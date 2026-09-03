/* ********************************************************************
 *								Copyright ©
 *    Copyright © 2002 Fonix Corporation. All rights reserved.
 *    Copyright © 2000-2001 Force Computers Inc., a Solectron company. All rights reserved.
 *
 *    Restricted Rights: Use, duplication, or disclosure by the U.S.
 *    Government is subject to restrictions as set forth in subparagraph
 *    (c) (1) (ii) of DFARS 252.227-7013, or in FAR 52.227-19, or in FAR
 *    52.227-14 Alt. III, as applicable.
 *
 *    This software is proprietary to and embodies the confidential
 *    technology of Fonix Corporation and other parties.
 *    Possession, use, or copying of this software and media is authorized
 *    only pursuant to a valid written license from Fonix or an
 *    authorized sublicensor.
 * *************************************************************************
 * Comments
 * install usa character set tables ...
 * rev	who		date		description
 * ------------------------------------------------------------------
 * 00?	GL		12/12/1996	need to change the usa_init() for local language.
 * 001	GL		04/21/1997	BATS#357  Add the code for __osf__ build
 * 002  DR		09/30/1997	UK BUILD: added UK STUFF
 * 003	MFG		06/19/1998	Made changed to include latin american
 * 004  ETT		10/05/1998  Added Linux code.
 * 005	MGS		04/13/2000  Changes for integrated phoneme set
 * 006  NAL     05/23/2000  Added function prototype and changed usa_init() type from int to
 *                          void(warning removal)
 * 007	CHJ		07/20/2000	French added
 * 008 	CAB		10/16/00	Changed copyright info
 * 009	MGS		03/27/2001	Fix compiler warning
 * 010	CAB		03/28/2001	Updated copyright info
 * 011	MGS		05/09/2001	Some VxWorks porting BATS#972
 * 012	MFG		05/29/2001	Included dectalkf.h
 * 013	MGS		06/19/2001	Solaris Port BATS#972
 * 014	MGS		04/11/2002	ARM7 port
 * 015	CAB		04/26/2002	Removed warnings by typecast
 */

#include "port.h"
#include "dectalkf.h"

#include <stdlib.h>

#include "usa_def.h"
#include "usa_type.tab"
#include "usa_phon.tab"
#include "usa_err.tab"

const unsigned char language_prefixes[] = {
    'u', 's'};

const int language_size = sizeof(language_prefixes);

const unsigned char* arpabet_arrays[] = {
    usa_arpa};

const unsigned int arpabet_sizes[] = {
    sizeof(usa_arpa)};

const unsigned int arpabet_lang_flags[] = {
    LANG_english};

const unsigned int arpabet_lang_fonts[] = {
    PFUSA};

/* MVP : The below variable "nlt" is now made local to usa_init function and
 * dynamically allocated to support multiple instances of speech object.
 */
/*struct  dtpc_language_tables nlt;*/

/*
 * #define USADEBUG_OLD 1
 */

extern void default_lang(PKSD_T, unsigned int, unsigned int);

void default_lang(PKSD_T, unsigned int, unsigned int); // NAL warning removal

/* ******************************************************************
 *  Function: usa_init()
 *
 *  Description:
 *
 *	Arguments:
 *		PKSD_T pKsd_t
 *
 *	Return Value:
 *		void
 *
 *	Comments:
 * *****************************************************************/
void usa_init(PKSD_T pKsd_t) {
	volatile struct dtpc_language_tables _far* lt;

	struct dtpc_language_tables* pnlt;
	/* nlt is allocated here ,It will be made free in DeleteTextToSpeechObject routine
	 * in API sub-system.
	 */
	if((pnlt = (struct dtpc_language_tables*)
		malloc(sizeof(struct dtpc_language_tables))) == NULL) {
		// MessageBox(NULL,"Error allocating dtpc_language_tables structure",
		//       Error",MB_OK);
		return; // (MMSYSERR_NOMEM);
	}

	/*
	 *  fill structure ...
	 */

#ifdef USADEBUG_OLD
	f_fprintf("In usa init\n");
#endif

	pnlt->link = NULL_LT;
	if(pKsd_t->lang_curr == LANG_english) {
		pnlt->lang_id = LANG_english;
		// CAB Removed warnings by typecast
		pnlt->lang_ascky      = (unsigned char*)usa_ascky;
		pnlt->lang_ascky_size = sizeof(usa_ascky);
		// CAB Removed warnings by typecast
		pnlt->lang_reverse_ascky = (unsigned int*)usa_ascky_rev;
		pnlt->lang_arpabet	 = (unsigned char*)usa_arpa;
		pnlt->lang_arpa_size	 = sizeof(usa_arpa);
		pnlt->lang_arpa_case	 = FALSE;
		pnlt->lang_typing	 = usa_type;
		pnlt->lang_error	 = usa_error;
	}


	/* GL 12/12/1996  set the language table */
	pKsd_t->ascky	      = pnlt->lang_ascky;
	pKsd_t->ascky_size    = pnlt->lang_ascky_size;
	pKsd_t->reverse_ascky = pnlt->lang_reverse_ascky;
	pKsd_t->arpabet	      = pnlt->lang_arpabet;
	pKsd_t->arpa_size     = pnlt->lang_arpa_size;
	pKsd_t->arpa_case     = pnlt->lang_arpa_case;
	pKsd_t->typing_table  = pnlt->lang_typing;
	pKsd_t->error_table   = pnlt->lang_error;

	/*
	 *  thread on chain ...
	 */

	lt = pKsd_t->loaded_languages;
	if(lt == NULL_LT)
		pKsd_t->loaded_languages = pnlt;
	else {
		while((*lt).link != NULL_LT)
			lt = (*lt).link;
		(*lt).link = pnlt;
	}
	/*
	 *  Install the language bit flag ...
	 */

#ifdef ENGLISH_US
	default_lang(pKsd_t, LANG_english, LANG_tables_ready);
#endif
#ifdef ENGLISH_UK
	default_lang(pKsd_t, LANG_british, LANG_tables_ready);
#endif
#ifdef SPANISH_SP
	default_lang(pKsd_t, LANG_spanish, LANG_tables_ready);
#endif
#ifdef SPANISH_LA
	default_lang(pKsd_t, LANG_latin_american, LANG_tables_ready);
#endif
#ifdef GERMAN
	default_lang(pKsd_t, LANG_german, LANG_tables_ready);
#endif
#ifdef FRENCH
	default_lang(pKsd_t, LANG_french, LANG_tables_ready);
#endif
}
