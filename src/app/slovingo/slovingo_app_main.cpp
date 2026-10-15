/****************************************************************************
 *   Slovingo app for the T-Watch: main tile (courses, sheets, exercises)
 *
 *   One tile, one scrollable page that is rebuilt for every screen:
 *   groups -> sheets -> sheet (read / exercises) -> reader | quiz -> result.
 *   The data comes from the export of src/export_api.py (index.json,
 *   sheets/<id>.json, exercises/<id>.json), downloaded by a worker task
 *   and cached in the SPIFFS, see slovingo_fetch.cpp.
 ****************************************************************************/

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NATIVE_64BIT
    #include <Arduino.h>
    #include <esp_system.h>
#endif

#include "slovingo_app.h"
#include "slovingo_app_main.h"
#include "slovingo_config.h"
#include "slovingo_fetch.h"

#include "gui/mainbar/app_tile/app_tile.h"
#include "gui/mainbar/main_tile/main_tile.h"
#include "gui/mainbar/mainbar.h"
#include "gui/statusbar.h"
#include "gui/keyboard.h"
#include "gui/widget_factory.h"
#include "gui/widget_styles.h"
#include "utils/alloc.h"

LV_FONT_DECLARE( slovingo_16px );
LV_FONT_DECLARE( slovingo_22px );

#ifdef NATIVE_64BIT
    #define SLV_RAND()      ( (uint32_t)rand() )
#else
    #define SLV_RAND()      ( esp_random() )
#endif

/*
 * margins of the text pages, tighter than the theme ones to give the text more room
 */
#define SLV_SIDE_MARGIN     2       /* px left and right of the text */
#define SLV_GAP             2       /* px above, below and between the lines */

/*
 * texts of the interface (the course content has its own language)
 */
#define TXT_LOADING         "Chargement..."
#define TXT_READ            "Lire la fiche"
#define TXT_EXERCISES       "Exercices"
#define TXT_NO_EXERCISES    "Pas d'exercice pour cette fiche"
#define SLV_ADVANCE_OK_MS   1000    /** @brief time a right answer stays on screen before the next question */
#define SLV_ADVANCE_KO_MS   2000    /** @brief time a wrong answer stays on screen (the right one is green) */
#define TXT_AGAIN           "Rejouer"
#define TXT_BACK            "Retour"
#define TXT_SAVE            "Enregistrer"
#define TXT_DEFAULT         "Valeur par défaut"
#define TXT_URL_HELP        "Adresse du dossier du cours (api) :"
#define TXT_COURSE          "Cours"

#define SLV_MAX_GROUPS      32
#define SLV_MAX_ITEMS       64
#define SLV_MAX_Q           64
#define SLV_MAX_CHOICES     4
#define SLV_TEXT_MAX        16384

typedef enum {
    SCR_LOADING,
    SCR_ERROR,
    SCR_GROUPS,
    SCR_SHEETS,
    SCR_SHEET,
    SCR_READER,
    SCR_QUIZ,
    SCR_RESULT,
    SCR_SETUP
} slv_screen_t;

typedef enum {
    REQ_INDEX,
    REQ_SHEET,
    REQ_EXERCISES
} slv_req_t;

typedef struct {
    char label[ 48 ];
    char cat[ 24 ];
    char sub[ 40 ];
    uint16_t count;
} slv_group_t;

typedef struct {
    char prompt[ 200 ];
    char hint[ 160 ];
    char choice[ SLV_MAX_CHOICES ][ 96 ];
    int choice_cnt;
    int correct;
} slv_question_t;

static slovingo_config_t slovingo_config;

/*
 * lvgl objects
 */
static lv_obj_t *slv_tile = NULL;
static lv_obj_t *slv_page = NULL;
static lv_obj_t *slv_exit_btn = NULL;
static lv_obj_t *slv_refresh_btn = NULL;
static lv_obj_t *slv_progress_lbl = NULL;   /* "x/x  Score: n" in the footer, quiz only */
static lv_obj_t *slv_setup_btn = NULL;
static lv_obj_t *slv_url_textarea = NULL;
static lv_task_t *slv_task = NULL;
static lv_style_t slv_style16;
static lv_style_t slv_style22;

static lv_obj_t *slv_item[ SLV_MAX_ITEMS ];         /** @brief clickable buttons of the current screen, in order */
static int slv_item_cnt = 0;

/*
 * screens
 */
static slv_screen_t slv_screen = SCR_LOADING;
static slv_screen_t slv_pending_screen = SCR_LOADING;
static bool slv_pending = false;
static slv_screen_t slv_screen_before_load = SCR_GROUPS;
static char slv_error_text[ 96 ] = "";

/*
 * course data, all in PSRAM
 */
static SpiRamJsonDocument *slv_index = NULL;
static SpiRamJsonDocument *slv_sheet = NULL;
static SpiRamJsonDocument *slv_ex = NULL;

static slv_group_t slv_groups[ SLV_MAX_GROUPS ];
static int slv_group_cnt = 0;
static int slv_cur_group = -1;
static int slv_sheet_idx[ SLV_MAX_ITEMS ];          /** @brief positions in index["sheets"] of the current group */
static int slv_sheet_cnt = 0;
static int slv_cur_sheet = -1;                      /** @brief position in index["sheets"] */

/*
 * worker: one request at a time
 */
static volatile bool slv_busy = false;
static volatile bool slv_done = false;
static slv_req_t slv_req = REQ_INDEX;
static char slv_req_path[ 96 ] = "index.json";
static bool slv_req_force = false;
static slovingo_result_t slv_res = SLOVINGO_OK;
static SpiRamJsonDocument *slv_res_doc = NULL;

/*
 * quiz
 */
static uint16_t slv_q_order[ SLV_MAX_Q ];
static int slv_q_cnt = 0;
static int slv_q_pos = 0;
static int slv_score = 0;
static bool slv_q_answered = false;
/* lv_tick time when the next question is shown after an answer, 0 = nothing scheduled */
static uint32_t slv_advance_at = 0;
static slv_question_t slv_q;

static void slv_show( slv_screen_t screen );
static void slv_goto( slv_screen_t screen );
static void slv_request( slv_req_t req, const char *path, bool force );
static void slv_app_activate_cb( void );
static void slv_app_hibernate_cb( void );
static void slv_lv_task( lv_task_t * task );
static void slv_back_event_cb( lv_obj_t * obj, lv_event_t event );
static void slv_refresh_event_cb( lv_obj_t * obj, lv_event_t event );
static void slv_setup_event_cb( lv_obj_t * obj, lv_event_t event );
static void slv_item_event_cb( lv_obj_t * obj, lv_event_t event );
static void slv_textarea_event_cb( lv_obj_t * obj, lv_event_t event );

/* ------------------------------------------------------------------------
 * small helpers
 * ---------------------------------------------------------------------- */

static lv_coord_t slv_content_w( void ) {
    return( lv_disp_get_hor_res( NULL ) - 2 * SLV_SIDE_MARGIN );
}

/*
 * label with the slovingo font, text is cleaned up for the font
 */
static lv_obj_t *slv_add_label( const char *text, bool big ) {
    char buffer[ 512 ];

    slovingo_sanitize( text, buffer, sizeof( buffer ) );
    lv_obj_t *label = wf_add_label( slv_page, buffer );
    lv_label_set_long_mode( label, LV_LABEL_LONG_BREAK );
    lv_obj_set_width( label, slv_content_w() );
    lv_obj_add_style( label, LV_LABEL_PART_MAIN, big ? &slv_style22 : &slv_style16 );
    return( label );
}

/*
 * button with a wrapped label, it is registered as item of the screen
 */
static lv_obj_t *slv_add_button( const char *text ) {
    char buffer[ 256 ];

    slovingo_sanitize( text, buffer, sizeof( buffer ) );
    lv_obj_t *btn = lv_btn_create( slv_page, NULL );
    lv_obj_set_width( btn, slv_content_w() );
    lv_btn_set_fit2( btn, LV_FIT_NONE, LV_FIT_TIGHT );
    lv_obj_add_style( btn, LV_BTN_PART_MAIN, ws_get_button_style() );
    lv_obj_set_event_cb( btn, slv_item_event_cb );

    lv_obj_t *label = lv_label_create( btn, NULL );
    lv_label_set_long_mode( label, LV_LABEL_LONG_BREAK );
    lv_obj_set_width( label, slv_content_w() - 2 * THEME_PADDING );
    lv_label_set_align( label, LV_LABEL_ALIGN_CENTER );
    lv_label_set_text( label, buffer );
    lv_obj_add_style( label, LV_LABEL_PART_MAIN, &slv_style16 );

    if ( slv_item_cnt < SLV_MAX_ITEMS )
        slv_item[ slv_item_cnt++ ] = btn;
    return( btn );
}

static int slv_item_index( lv_obj_t *obj ) {
    for ( int i = 0; i < slv_item_cnt; i++ ) {
        if ( slv_item[ i ] == obj )
            return( i );
    }
    return( -1 );
}

/*
 * append a cleaned up piece of text, a piece that does not fit is dropped
 */
static void slv_cat( char *dst, size_t cap, size_t *len, const char *text ) {
    char piece[ 384 ];

    if ( !text )
        return;
    slovingo_sanitize( text, piece, sizeof( piece ) );
    size_t n = strlen( piece );
    if ( *len + n + 1 > cap )
        return;
    memcpy( dst + *len, piece, n );
    *len += n;
    dst[ *len ] = '\0';
}

/* ------------------------------------------------------------------------
 * groups and sheets of the index
 * ---------------------------------------------------------------------- */

static void slv_build_groups( void ) {
    slv_group_cnt = 0;
    slv_cur_group = -1;
    slv_cur_sheet = -1;

    if ( !slv_index )
        return;

    JsonArray sheets = (*slv_index)["sheets"];
    for ( JsonObject sheet : sheets ) {
        const char *cat = sheet["cat"] | "";
        const char *sub = sheet["sub"] | "";
        int found = -1;

        for ( int i = 0; i < slv_group_cnt; i++ ) {
            if ( !strcmp( slv_groups[ i ].cat, cat ) && !strcmp( slv_groups[ i ].sub, sub ) ) {
                found = i;
                break;
            }
        }
        if ( found < 0 ) {
            if ( slv_group_cnt >= SLV_MAX_GROUPS )
                continue;
            found = slv_group_cnt++;
            slv_group_t *group = &slv_groups[ found ];
            snprintf( group->cat, sizeof( group->cat ), "%s", cat );
            snprintf( group->sub, sizeof( group->sub ), "%s", sub );
            const char *label = sheet["sub_label"] | "";
            if ( *label ) {
                snprintf( group->label, sizeof( group->label ), "%s", label );
            }
            else {
                snprintf( group->label, sizeof( group->label ), "%s", cat );
                if ( group->label[ 0 ] >= 'a' && group->label[ 0 ] <= 'z' )
                    group->label[ 0 ] -= 'a' - 'A';
            }
            group->count = 0;
        }
        slv_groups[ found ].count++;
    }
}

static void slv_collect_sheets( int group ) {
    slv_sheet_cnt = 0;
    if ( !slv_index || group < 0 || group >= slv_group_cnt )
        return;

    int pos = 0;
    JsonArray sheets = (*slv_index)["sheets"];
    for ( JsonObject sheet : sheets ) {
        const char *cat = sheet["cat"] | "";
        const char *sub = sheet["sub"] | "";
        if ( !strcmp( slv_groups[ group ].cat, cat ) && !strcmp( slv_groups[ group ].sub, sub ) && slv_sheet_cnt < SLV_MAX_ITEMS )
            slv_sheet_idx[ slv_sheet_cnt++ ] = pos;
        pos++;
    }
}

static JsonObject slv_cur_sheet_obj( void ) {
    return( (*slv_index)["sheets"][ slv_cur_sheet ] );
}

/* exercises the watch can play: qcm and fill-blank (no audio, no drag and drop) */
static int slv_playable_count( JsonObject sheet ) {
    JsonObject ex = sheet["ex"];
    return( ex["qcm"].as<int>() + ex["fill-blank"].as<int>() );
}

/* ------------------------------------------------------------------------
 * reader: sheet blocks -> plain text
 * ---------------------------------------------------------------------- */

static char *slv_build_reader_text( void ) {
    char *text = (char*)MALLOC( SLV_TEXT_MAX );
    size_t len = 0;

    if ( !text )
        return( NULL );
    text[ 0 ] = '\0';

    JsonArray blocks = (*slv_sheet)["blocks"];
    for ( JsonObject block : blocks ) {
        const char *type = block["t"] | "";

        if ( !strcmp( type, "h" ) ) {
            slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            slv_cat( text, SLV_TEXT_MAX, &len, block["x"] | "" );
            slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
        }
        else if ( !strcmp( type, "p" ) ) {
            slv_cat( text, SLV_TEXT_MAX, &len, block["x"] | "" );
            slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
        }
        else if ( !strcmp( type, "table" ) ) {
            JsonArray rows = block["rows"];
            for ( JsonArray row : rows ) {
                bool first = true;
                for ( JsonVariant cell : row ) {
                    slv_cat( text, SLV_TEXT_MAX, &len, first ? "" : " = " );
                    slv_cat( text, SLV_TEXT_MAX, &len, cell.as<const char*>() );
                    first = false;
                }
                slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            }
        }
        else if ( !strcmp( type, "card" ) ) {
            slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            slv_cat( text, SLV_TEXT_MAX, &len, block["x"] | "" );
            slv_cat( text, SLV_TEXT_MAX, &len, "\n   " );
            slv_cat( text, SLV_TEXT_MAX, &len, block["n"] | "" );
            slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            JsonArray literal = block["lit"];
            for ( JsonVariant line : literal ) {
                slv_cat( text, SLV_TEXT_MAX, &len, "   - " );
                slv_cat( text, SLV_TEXT_MAX, &len, line.as<const char*>() );
                slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            }
            JsonArray notes = block["nt"];
            for ( JsonVariant line : notes ) {
                slv_cat( text, SLV_TEXT_MAX, &len, "   * " );
                slv_cat( text, SLV_TEXT_MAX, &len, line.as<const char*>() );
                slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            }
        }
        else if ( !strcmp( type, "list" ) ) {
            JsonArray items = block["items"];
            for ( JsonVariant item : items ) {
                slv_cat( text, SLV_TEXT_MAX, &len, "- " );
                slv_cat( text, SLV_TEXT_MAX, &len, item.as<const char*>() );
                slv_cat( text, SLV_TEXT_MAX, &len, "\n" );
            }
        }
    }
    return( text );
}

/* ------------------------------------------------------------------------
 * quiz
 * ---------------------------------------------------------------------- */

static void slv_q_add_choice( const char *text ) {
    if ( !text || !*text || slv_q.choice_cnt >= SLV_MAX_CHOICES )
        return;
    for ( int i = 0; i < slv_q.choice_cnt; i++ ) {
        if ( !strcmp( slv_q.choice[ i ], text ) )
            return;
    }
    snprintf( slv_q.choice[ slv_q.choice_cnt++ ], sizeof( slv_q.choice[ 0 ] ), "%s", text );
}

static void slv_q_add_distractors( JsonArray distractors ) {
    for ( JsonVariant value : distractors )
        slv_q_add_choice( value.as<const char*>() );
}

/*
 * qcm: ask in one language, answer in the other, direction picked at random
 * fill-blank: the target sentence with its missing word hidden
 */
static void slv_make_question( int ex_idx ) {
    JsonObject exercise = (*slv_ex)["exercises"][ ex_idx ];
    const char *type = exercise["type"] | "";

    memset( &slv_q, 0, sizeof( slv_q ) );

    if ( !strcmp( type, "fill-blank" ) ) {
        const char *sentence = exercise["l2"] | "";
        const char *missing = exercise["missing_l2"] | "";
        const char *pos = *missing ? strstr( sentence, missing ) : NULL;

        if ( pos )
            snprintf( slv_q.prompt, sizeof( slv_q.prompt ), "%.*s____%s", (int)( pos - sentence ), sentence, pos + strlen( missing ) );
        else
            snprintf( slv_q.prompt, sizeof( slv_q.prompt ), "%s ____", sentence );
        if ( exercise["show_translation"] | true )
            snprintf( slv_q.hint, sizeof( slv_q.hint ), "%s", exercise["l1"] | "" );
        slv_q_add_choice( missing );
        slv_q_add_distractors( exercise["choices_l2"] );
    }
    else {
        bool to_target = ( SLV_RAND() & 1 ) != 0;

        snprintf( slv_q.prompt, sizeof( slv_q.prompt ), "%s", exercise[ to_target ? "l1" : "l2" ] | "" );
        slv_q_add_choice( exercise[ to_target ? "l2" : "l1" ] | "" );
        slv_q_add_distractors( exercise[ to_target ? "choices_l2" : "choices_l1" ] );
    }

    /* the right answer was added first, shuffle and follow it */
    slv_q.correct = 0;
    for ( int i = slv_q.choice_cnt - 1; i > 0; i-- ) {
        int j = SLV_RAND() % ( i + 1 );
        if ( i == j )
            continue;
        char tmp[ sizeof( slv_q.choice[ 0 ] ) ];
        memcpy( tmp, slv_q.choice[ i ], sizeof( tmp ) );
        memcpy( slv_q.choice[ i ], slv_q.choice[ j ], sizeof( tmp ) );
        memcpy( slv_q.choice[ j ], tmp, sizeof( tmp ) );
        if ( slv_q.correct == i )
            slv_q.correct = j;
        else if ( slv_q.correct == j )
            slv_q.correct = i;
    }
    slv_q_answered = false;
}

static bool slv_quiz_start( void ) {
    int total = 0;
    int pos = 0;

    if ( !slv_ex )
        return( false );

    JsonArray exercises = (*slv_ex)["exercises"];
    for ( JsonObject exercise : exercises ) {
        const char *type = exercise["type"] | "";
        if ( ( !strcmp( type, "qcm" ) || !strcmp( type, "fill-blank" ) ) && total < SLV_MAX_Q )
            slv_q_order[ total++ ] = pos;
        pos++;
    }
    if ( total == 0 )
        return( false );

    for ( int i = total - 1; i > 0; i-- ) {
        int j = SLV_RAND() % ( i + 1 );
        uint16_t tmp = slv_q_order[ i ];
        slv_q_order[ i ] = slv_q_order[ j ];
        slv_q_order[ j ] = tmp;
    }

    slv_q_cnt = total < slovingo_config.questions ? total : slovingo_config.questions;
    slv_q_pos = 0;
    slv_score = 0;
    slv_make_question( slv_q_order[ 0 ] );
    return( true );
}

static void slv_answer( int choice ) {
    if ( slv_q_answered || choice < 0 || choice >= slv_q.choice_cnt )
        return;

    slv_q_answered = true;
    if ( choice == slv_q.correct )
        slv_score++;

    lv_obj_set_style_local_bg_color( slv_item[ slv_q.correct ], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex( 0x2E7D32 ) );
    if ( choice != slv_q.correct )
        lv_obj_set_style_local_bg_color( slv_item[ choice ], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex( 0xC62828 ) );

    /* no next button: the next question comes by itself, a wrong answer stays on screen a little longer */
    slv_advance_at = lv_tick_get() + ( choice == slv_q.correct ? SLV_ADVANCE_OK_MS : SLV_ADVANCE_KO_MS );
}

static void slv_next_question( void ) {
    slv_q_pos++;
    if ( slv_q_pos >= slv_q_cnt ) {
        slv_goto( SCR_RESULT );
        return;
    }
    slv_make_question( slv_q_order[ slv_q_pos ] );
    slv_goto( SCR_QUIZ );
}

/* ------------------------------------------------------------------------
 * worker
 * ---------------------------------------------------------------------- */

static void slv_worker( void *arg ) {
    SpiRamJsonDocument *doc = NULL;

    slv_res = slovingo_fetch_json( slovingo_config.url, slv_req_path, slv_req_force, &doc );
    slv_res_doc = doc;
    slv_done = true;
#ifndef NATIVE_64BIT
    vTaskDelete( NULL );
#endif
}

static void slv_request( slv_req_t req, const char *path, bool force ) {
    if ( slv_busy )
        return;

    char copy[ sizeof( slv_req_path ) ];
    snprintf( copy, sizeof( copy ), "%s", path );

    slv_req = req;
    snprintf( slv_req_path, sizeof( slv_req_path ), "%s", copy );
    slv_req_force = force;
    slv_busy = true;
    slv_done = false;
    if ( slv_screen != SCR_LOADING )
        slv_screen_before_load = slv_screen;
    slv_goto( SCR_LOADING );

#ifdef NATIVE_64BIT
    slv_worker( NULL );
#else
    /* the tls handshake needs a big stack */
    if ( xTaskCreatePinnedToCore( slv_worker, "slovingo worker", 12288, NULL, 1, NULL, 1 ) != pdPASS ) {
        slv_busy = false;
        slv_res = SLOVINGO_ERR_MEM;
        snprintf( slv_error_text, sizeof( slv_error_text ), "%s", slovingo_result_text( SLOVINGO_ERR_MEM ) );
        slv_goto( SCR_ERROR );
    }
#endif
}

/*
 * lvgl task: applies the screen changes asked by the callbacks, and takes the
 * result of the worker
 */
static void slv_lv_task( lv_task_t * task ) {
    if ( slv_pending ) {
        slv_pending = false;
        slv_show( slv_pending_screen );
    }

    if ( slv_advance_at && slv_screen == SCR_QUIZ && slv_q_answered && lv_tick_get() >= slv_advance_at ) {
        slv_advance_at = 0;
        slv_next_question();
    }

    if ( !slv_done )
        return;

    slv_done = false;
    slv_busy = false;
    SpiRamJsonDocument *doc = slv_res_doc;
    slv_res_doc = NULL;

    if ( slv_res != SLOVINGO_OK || !doc ) {
        delete doc;
        snprintf( slv_error_text, sizeof( slv_error_text ), "%s", slovingo_result_text( slv_res ) );
        slv_show( SCR_ERROR );
        return;
    }

    switch ( slv_req ) {
        case REQ_INDEX:         delete slv_index;
                                slv_index = doc;
                                slv_build_groups();
                                slv_show( SCR_GROUPS );
                                break;
        case REQ_SHEET:         delete slv_sheet;
                                slv_sheet = doc;
                                slv_show( SCR_READER );
                                break;
        case REQ_EXERCISES:     delete slv_ex;
                                slv_ex = doc;
                                if ( slv_quiz_start() ) {
                                    slv_show( SCR_QUIZ );
                                }
                                else {
                                    snprintf( slv_error_text, sizeof( slv_error_text ), "%s", TXT_NO_EXERCISES );
                                    slv_show( SCR_ERROR );
                                }
                                break;
    }
}

/* ------------------------------------------------------------------------
 * screens
 * ---------------------------------------------------------------------- */

static void slv_goto( slv_screen_t screen ) {
    slv_pending_screen = screen;
    slv_pending = true;
}

static void slv_show( slv_screen_t screen ) {
    char text[ 160 ];

    slv_screen = screen;
    slv_pending = false;
    slv_advance_at = 0;
    slv_item_cnt = 0;
    slv_url_textarea = NULL;
    lv_page_clean( slv_page );

    lv_obj_set_hidden( slv_refresh_btn, !( screen == SCR_GROUPS || screen == SCR_ERROR ) );
    lv_obj_set_hidden( slv_setup_btn, screen != SCR_GROUPS );
    lv_obj_set_hidden( slv_progress_lbl, screen != SCR_QUIZ );

    switch ( screen ) {
        case SCR_LOADING:
            slv_add_label( TXT_LOADING, true );
            break;

        case SCR_ERROR:
            slv_add_label( slv_error_text, false );
            break;

        case SCR_GROUPS:
            slv_add_label( slv_index ? ( (*slv_index)["title"] | TXT_COURSE ) : TXT_COURSE, true );
            for ( int i = 0; i < slv_group_cnt; i++ ) {
                snprintf( text, sizeof( text ), "%s (%d)", slv_groups[ i ].label, slv_groups[ i ].count );
                slv_add_button( text );
            }
            break;

        case SCR_SHEETS:
            slv_collect_sheets( slv_cur_group );
            slv_add_label( slv_cur_group >= 0 ? slv_groups[ slv_cur_group ].label : "", true );
            for ( int i = 0; i < slv_sheet_cnt; i++ ) {
                JsonObject sheet = (*slv_index)["sheets"][ slv_sheet_idx[ i ] ];
                slv_add_button( sheet["title"] | "?" );
            }
            break;

        case SCR_SHEET: {
            JsonObject sheet = slv_cur_sheet_obj();
            int playable = slv_playable_count( sheet );

            slv_add_label( sheet["title"] | "?", false );
            slv_add_button( TXT_READ );
            if ( playable > 0 ) {
                snprintf( text, sizeof( text ), "%s (%d)", TXT_EXERCISES, playable );
                slv_add_button( text );
            }
            else {
                slv_add_label( TXT_NO_EXERCISES, false );
            }
            break;
        }

        case SCR_READER: {
            JsonObject sheet = slv_cur_sheet_obj();
            char *body = slv_build_reader_text();

            slv_add_label( sheet["title"] | "?", true );
            if ( body ) {
                lv_obj_t *label = wf_add_label( slv_page, body );
                lv_label_set_long_mode( label, LV_LABEL_LONG_BREAK );
                lv_obj_set_width( label, slv_content_w() );
                lv_obj_add_style( label, LV_LABEL_PART_MAIN, &slv_style16 );
                free( body );
            }
            else {
                slv_add_label( slovingo_result_text( SLOVINGO_ERR_MEM ), false );
            }
            break;
        }

        case SCR_QUIZ:
            snprintf( text, sizeof( text ), "%d/%d  Score: %d", slv_q_pos + 1, slv_q_cnt, slv_score );
            lv_label_set_text( slv_progress_lbl, text );
            slv_add_label( slv_q.prompt, true );
            if ( *slv_q.hint )
                slv_add_label( slv_q.hint, false );
            for ( int i = 0; i < slv_q.choice_cnt; i++ )
                slv_add_button( slv_q.choice[ i ] );
            break;

        case SCR_RESULT:
            snprintf( text, sizeof( text ), "Score : %d / %d", slv_score, slv_q_cnt );
            slv_add_label( text, true );
            slv_add_button( TXT_AGAIN );
            slv_add_button( TXT_BACK );
            break;

        case SCR_SETUP:
            slv_add_label( TXT_URL_HELP, false );
            slv_url_textarea = lv_textarea_create( slv_page, NULL );
            lv_textarea_set_text( slv_url_textarea, slovingo_config.url );
            lv_textarea_set_max_length( slv_url_textarea, SLOVINGO_URL_LEN - 1 );
            lv_textarea_set_one_line( slv_url_textarea, true );
            lv_textarea_set_cursor_hidden( slv_url_textarea, true );
            lv_obj_set_width( slv_url_textarea, slv_content_w() );
            lv_obj_set_event_cb( slv_url_textarea, slv_textarea_event_cb );
            lv_tileview_add_element( slv_tile, slv_url_textarea );
            slv_add_button( TXT_SAVE );
            slv_add_button( TXT_DEFAULT );
            break;
    }

    for ( int i = 0; i < slv_item_cnt; i++ )
        lv_tileview_add_element( slv_tile, slv_item[ i ] );
    lv_obj_set_y( lv_page_get_scrollable( slv_page ), 0 );
}

/* ------------------------------------------------------------------------
 * events
 * ---------------------------------------------------------------------- */

static void slv_open_group( int item ) {
    if ( item < 0 || item >= slv_group_cnt )
        return;
    slv_cur_group = item;
    slv_goto( SCR_SHEETS );
}

static void slv_open_sheet( int item ) {
    if ( item < 0 || item >= slv_sheet_cnt )
        return;
    slv_cur_sheet = slv_sheet_idx[ item ];
    delete slv_sheet;
    slv_sheet = NULL;
    delete slv_ex;
    slv_ex = NULL;
    slv_goto( SCR_SHEET );
}

static void slv_open_data( slv_req_t req ) {
    char path[ 96 ];
    JsonObject sheet = slv_cur_sheet_obj();

    snprintf( path, sizeof( path ), "%s/%s.json", req == REQ_SHEET ? "sheets" : "exercises", (const char*)( sheet["id"] | "" ) );
    slv_request( req, path, false );
}

static void slv_item_event_cb( lv_obj_t * obj, lv_event_t event ) {
    if ( event != LV_EVENT_CLICKED )
        return;

    int item = slv_item_index( obj );
    if ( item < 0 )
        return;

    switch ( slv_screen ) {
        case SCR_GROUPS:    slv_open_group( item );
                            break;
        case SCR_SHEETS:    slv_open_sheet( item );
                            break;
        case SCR_SHEET:     if ( item == 0 )
                                slv_open_data( REQ_SHEET );
                            else
                                slv_open_data( REQ_EXERCISES );
                            break;
        case SCR_QUIZ:      slv_answer( item );
                            break;
        case SCR_RESULT:    if ( item == 0 ) {
                                if ( slv_quiz_start() )
                                    slv_goto( SCR_QUIZ );
                            }
                            else {
                                slv_goto( SCR_SHEET );
                            }
                            break;
        case SCR_SETUP:     if ( item == 0 ) {
                                if ( slv_url_textarea )
                                    snprintf( slovingo_config.url, sizeof( slovingo_config.url ), "%s", lv_textarea_get_text( slv_url_textarea ) );
                            }
                            else {
                                snprintf( slovingo_config.url, sizeof( slovingo_config.url ), "%s", SLOVINGO_DEFAULT_URL );
                            }
                            keyboard_hide();
                            slovingo_config.save();
                            delete slv_index;
                            slv_index = NULL;
                            delete slv_sheet;
                            slv_sheet = NULL;
                            delete slv_ex;
                            slv_ex = NULL;
                            slv_build_groups();
                            slv_request( REQ_INDEX, "index.json", true );
                            break;
        default:            break;
    }
}

static void slv_back_event_cb( lv_obj_t * obj, lv_event_t event ) {
    if ( event != LV_EVENT_CLICKED )
        return;

    keyboard_hide();

    switch ( slv_screen ) {
        case SCR_LOADING:   /* the worker cannot be stopped, wait for its answer */
                            break;
        case SCR_GROUPS:    mainbar_jump_back();
                            break;
        case SCR_SHEETS:    slv_goto( SCR_GROUPS );
                            break;
        case SCR_SHEET:     slv_goto( SCR_SHEETS );
                            break;
        case SCR_READER:
        case SCR_QUIZ:
        case SCR_RESULT:    slv_goto( SCR_SHEET );
                            break;
        case SCR_SETUP:     slv_goto( slv_index ? SCR_GROUPS : SCR_ERROR );
                            break;
        case SCR_ERROR:     if ( !slv_index )
                                mainbar_jump_back();
                            else
                                slv_goto( slv_screen_before_load == SCR_LOADING ? SCR_GROUPS : slv_screen_before_load );
                            break;
    }
}

static void slv_refresh_event_cb( lv_obj_t * obj, lv_event_t event ) {
    if ( event != LV_EVENT_CLICKED )
        return;

    if ( slv_screen == SCR_ERROR )
        slv_request( slv_req, slv_req_path, true );
    else if ( slv_screen == SCR_GROUPS )
        slv_request( REQ_INDEX, "index.json", true );
}

static void slv_setup_event_cb( lv_obj_t * obj, lv_event_t event ) {
    if ( event != LV_EVENT_CLICKED )
        return;
    if ( slv_screen == SCR_GROUPS )
        slv_goto( SCR_SETUP );
}

static void slv_textarea_event_cb( lv_obj_t * obj, lv_event_t event ) {
    if ( event == LV_EVENT_CLICKED )
        keyboard_set_textarea( obj );
}

static void slv_app_activate_cb( void ) {
    lv_task_set_prio( slv_task, LV_TASK_PRIO_MID );

    if ( !slv_index && !slv_busy ) {
        slv_screen = SCR_GROUPS;
        slv_request( REQ_INDEX, "index.json", false );
    }
    else if ( slv_screen == SCR_SHEET && !slv_pending ) {
        slv_goto( SCR_SHEET );
    }
}

static void slv_app_hibernate_cb( void ) {
    keyboard_hide();
    slv_advance_at = 0;

    /* the big documents go, the index stays, the screens rebuild from it */
    delete slv_sheet;
    slv_sheet = NULL;
    delete slv_ex;
    slv_ex = NULL;
    if ( slv_screen == SCR_READER || slv_screen == SCR_QUIZ || slv_screen == SCR_RESULT )
        slv_goto( SCR_SHEET );
    else if ( slv_screen == SCR_SETUP )
        slv_goto( slv_index ? SCR_GROUPS : SCR_ERROR );
}

void slovingo_app_main_setup( uint32_t tile_num ) {

    slovingo_config.load();

    mainbar_add_tile_activate_cb( tile_num, slv_app_activate_cb );
    mainbar_add_tile_hibernate_cb( tile_num, slv_app_hibernate_cb );

    slv_tile = mainbar_get_tile_obj( tile_num );

    lv_style_init( &slv_style16 );
    lv_style_set_text_font( &slv_style16, LV_STATE_DEFAULT, &slovingo_16px );
    lv_style_init( &slv_style22 );
    lv_style_set_text_font( &slv_style22, LV_STATE_DEFAULT, &slovingo_22px );

    lv_coord_t hor_res = lv_disp_get_hor_res( NULL );
    lv_coord_t ver_res = lv_disp_get_ver_res( NULL );

    /*
     * the page fills the tile down to the footer, its content is rebuilt by slv_show()
     */
    slv_page = lv_page_create( slv_tile, NULL );
    lv_obj_set_size( slv_page, hor_res, ver_res - THEME_ICON_SIZE );
    lv_obj_align( slv_page, slv_tile, LV_ALIGN_IN_TOP_MID, 0, 0 );
    lv_obj_add_style( slv_page, LV_OBJ_PART_MAIN, APP_STYLE );
    lv_page_set_scrlbar_mode( slv_page, LV_SCRLBAR_MODE_DRAG );
    lv_page_set_scrl_layout( slv_page, LV_LAYOUT_COLUMN_MID );
    lv_obj_set_style_local_pad_top( slv_page, LV_PAGE_PART_SCROLLABLE, LV_STATE_DEFAULT, SLV_GAP );
    lv_obj_set_style_local_pad_bottom( slv_page, LV_PAGE_PART_SCROLLABLE, LV_STATE_DEFAULT, SLV_GAP );
    lv_obj_set_style_local_pad_inner( slv_page, LV_PAGE_PART_SCROLLABLE, LV_STATE_DEFAULT, SLV_GAP );

    slv_exit_btn = wf_add_exit_button( slv_tile, slv_back_event_cb );
    lv_obj_align( slv_exit_btn, slv_tile, LV_ALIGN_IN_BOTTOM_LEFT, THEME_ICON_PADDING, -THEME_ICON_PADDING );

    /* quiz progress, right of the exit arrow (same row, the refresh button is hidden in the quiz) */
    slv_progress_lbl = lv_label_create( slv_tile, NULL );
    lv_label_set_long_mode( slv_progress_lbl, LV_LABEL_LONG_CROP );
    lv_obj_set_width( slv_progress_lbl, hor_res - 2 * THEME_ICON_SIZE - 2 * THEME_ICON_PADDING );
    lv_obj_add_style( slv_progress_lbl, LV_LABEL_PART_MAIN, &slv_style16 );
    lv_obj_align( slv_progress_lbl, slv_exit_btn, LV_ALIGN_OUT_RIGHT_MID, THEME_PADDING, 0 );
    lv_obj_set_hidden( slv_progress_lbl, true );

    slv_refresh_btn = wf_add_refresh_button( slv_tile, slv_refresh_event_cb );
    lv_obj_align( slv_refresh_btn, slv_tile, LV_ALIGN_IN_BOTTOM_MID, 0, -THEME_ICON_PADDING );

    slv_setup_btn = wf_add_setup_button( slv_tile, slv_setup_event_cb );
    lv_obj_align( slv_setup_btn, slv_tile, LV_ALIGN_IN_BOTTOM_RIGHT, -THEME_ICON_PADDING, -THEME_ICON_PADDING );

    lv_tileview_add_element( slv_tile, slv_page );
    lv_tileview_add_element( slv_tile, slv_exit_btn );
    lv_tileview_add_element( slv_tile, slv_refresh_btn );
    lv_tileview_add_element( slv_tile, slv_setup_btn );
    lv_tileview_add_element( slv_tile, slv_progress_lbl );

    slv_task = lv_task_create( slv_lv_task, 100, LV_TASK_PRIO_OFF, NULL );

    slv_show( SCR_LOADING );
}
