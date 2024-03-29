//*******************************************************************************
//
// This file is part of the OpenHoldem project
//   Download page:         http://code.google.com/p/openholdembot/
//   Forums:                http://www.maxinmontreal.com/forums/index.php
//   Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//*******************************************************************************
//
// Purpose:
//
//*******************************************************************************

#include "StdAfx.h"
#include "CPokerTrackerThread.h"

#include <assert.h>
#include <process.h>
#include <comdef.h>
#include "CAutoConnector.h"
#include "CGameState.h"
#include "CLevDistance.h"
#include "..\PokerTracker_Query_Definitions\pokertracker_query_definitions.h"
#include "CPokerTrackerLookUp.h"
#include "CPreferences.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineAutoplayer.h"
#include "CSymbolEngineIsOmaha.h"
#include "CSymbolEngineIsTournament.h"
#include "CSymbolEnginePokerTracker.h"
#include "CSymbolEngineTime.h"
#include "CSymbolEngineUserchair.h"
#include "..\CTablemap\CTablemap.h"
#include "MagicNumbers.h"

CPokerTrackerThread	*p_pokertracker_thread = NULL;

SPlayerData _player_data[k_max_number_of_players];


CString CPokerTrackerThread::CreateConnectionString(
	const CString ip_address, const CString port, const CString username,
	const CString password, const CString DB_name)
{
	CString result;
	result.Format("host=%s port=%s user=%s password=%s dbname='%s'",  
		ip_address, port, username, password, DB_name);
	write_log(preferences.debug_pokertracker(), "[PokerTracker] Connection-string: %s\n", result);
	return result;
}


CPokerTrackerThread::CPokerTrackerThread()
{
	// Initialize variables
	_pt_thread = NULL;

	_conn_str = CreateConnectionString(preferences.pt_ip_addr(), 
		preferences.pt_port(), preferences.pt_user(), preferences.pt_pass(), preferences.pt_dbname());

	_connected = false;
	_m_stop_thread = NULL;
	_m_wait_thread = NULL;
	_pgconn = NULL; 
}

CPokerTrackerThread::~CPokerTrackerThread()
{
	StopThread();
}

void CPokerTrackerThread::StartThread()
{
	if (!AllConnectionDataSpecified())
	{
		return;
	}
	if (_pt_thread == NULL)
	{
		// Create events
		_m_stop_thread = CreateEvent(0, TRUE, FALSE, 0);
		_m_wait_thread = CreateEvent(0, TRUE, FALSE, 0);

		_pt_thread = AfxBeginThread(PokertrackerThreadFunction, this);

		write_log(preferences.debug_pokertracker(), "[PokerTracker] Started PokerTracker-thread.\n");
	}
	else
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] PokerTracker-thread already running.\n");
	}
}

void CPokerTrackerThread::StopThread()
{
	if (_pt_thread)
	{
		// Trigger thread to stop
		::SetEvent(_m_stop_thread);

		// Wait until thread finished
		::WaitForSingleObject(_m_wait_thread, k_max_time_to_wait_for_thread_to_shutdown);

		_pt_thread = NULL;

		Disconnect();

		write_log(preferences.debug_pokertracker(), "[PokerTracker] Stopped Poker Tracker thread.\n");
	}

	// Close handles
	if (_m_stop_thread)
	{
		::CloseHandle(_m_stop_thread);
		_m_stop_thread = NULL;
	}
	if (_m_wait_thread)
	{
		::CloseHandle(_m_wait_thread);
		_m_wait_thread = NULL;
	}
}

bool CPokerTrackerThread::AllConnectionDataSpecified()
{
	return(preferences.pt_ip_addr() != ""
		&& preferences.pt_port()    != ""
		&& preferences.pt_user()    != ""
		&& preferences.pt_pass()    != ""
		&& preferences.pt_dbname()  != "");


}

void CPokerTrackerThread::Connect(void) {
	write_log(preferences.debug_pokertracker(), "[PokerTracker] Trying to open PostgreSQL DB...\n");
	if (!AllConnectionDataSpecified()) 	{
    write_log(preferences.debug_pokertracker(), "[PokerTracker] Can't connect to DB. Vital data missing\n");
		return;
	}
	_conn_str = CreateConnectionString(preferences.pt_ip_addr(), 
		preferences.pt_port(), preferences.pt_user(), preferences.pt_pass(), preferences.pt_dbname());
	_pgconn = PQconnectdb(_conn_str.GetString());

	if (PQstatus(_pgconn) == CONNECTION_OK)	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] PostgreSQL DB opened successfully <%s/%s/%s>\n", 
			preferences.pt_ip_addr(), 
			preferences.pt_port(), 
			preferences.pt_dbname());

		_connected = true;
	}	else {
		write_log(preferences.debug_pokertracker(), "[PokerTracker] ERROR opening PostgreSQL DB: %s\n\n", PQerrorMessage(_pgconn));
		PQfinish(_pgconn);
		_connected = false;
		_pgconn = NULL;
	}
}

void CPokerTrackerThread::Disconnect(void)
{
	if(_pgconn)
    {
      if (PQstatus(_pgconn) == CONNECTION_OK)
        {
          PQfinish(_pgconn);
        }
    }
	_pgconn = NULL;
	_connected = false;
}

bool CPokerTrackerThread::IsConnected()
{
  write_log(preferences.debug_pokertracker(), "[PokerTracker] connected: %s\n",
    Bool2CString(_connected));
  write_log(preferences.debug_pokertracker(), "[PokerTracker]PXStatus = %d (0 = CONNECTION_OK)\n",
    PQstatus(_pgconn));
	return (_connected && PQstatus(_pgconn) == CONNECTION_OK);
}

/* A scraped name is "bad" if it consists only of characters 
   like "l", "1", "i", "." and "," */
bool CPokerTrackerThread::NameLooksLikeBadScrape(char *oh_scraped_name)
{
	int len = (int) strlen(oh_scraped_name);
	for (int i=0; i<len; i++)
	{
		if (oh_scraped_name[i]    != 'l'
			&& oh_scraped_name[i] != '1'
			&& oh_scraped_name[i] != 'i' 
			&& oh_scraped_name[i] != '.' 
			&& oh_scraped_name[i] != ',')
		{
			// Good name, good character found, no bad scrape
			return false;
		}
	}
	// Bad name, only bad characters found, bad scrape
	return true;
}

/* Returns true if found an appropriate name in the DB for chr, or false if 
   it did not found such name.*/
bool CPokerTrackerThread::CheckIfNameExistsInDB(int chair)
{
	char		oh_scraped_name[k_max_length_of_playername]; 
	char		best_name[k_max_length_of_playername];

	assert(chair >= k_first_chair); 
	assert(chair <= k_last_chair);
	
	memset(oh_scraped_name, 0, k_max_length_of_playername);
	memset(best_name, 0, k_max_length_of_playername);

	write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() chair = %i\n", chair);
	
	if (p_game_state->state((p_game_state->state_index()-1)&0xff)->m_player[chair].m_name_known == 0)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() No name known for this chair\n");
		return false;
	}

	if (strlen(p_game_state->state((p_game_state->state_index()-1)&0xff)->m_player[chair].m_name) <= k_max_length_of_playername)
	{
		strcpy_s(oh_scraped_name, k_max_length_of_playername, p_game_state->state((p_game_state->state_index()-1)&0xff)->m_player[chair].m_name);
	}

	write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() Scraped name: [%s]\n", oh_scraped_name);

	if (NameLooksLikeBadScrape(oh_scraped_name))
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() Name looks like a bad scrape\n");
		return false;
	}
	
	// We already have the name, and it has not changed since we last checked, so do nothing
	if (_player_data[chair].found && 0 == strcmp(_player_data[chair].scraped_name, oh_scraped_name))
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() Name is known and good\n");
		return true;
	}
	
	// We have not found the name in PT, go find it
	// First see if we can find the exact scraped name
	if (FindName(oh_scraped_name, best_name))
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() Name found in database\n");
		SetPlayerName(chair, true, best_name, oh_scraped_name);
		return true;
	}
	else
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] CheckIfNameExistsInDB() Name not found in database\n");
		SetPlayerName(chair, false, "", "");
		return false;
	}
}

/* Returns true, if the name looks ok, but differs from the last one. */
bool CPokerTrackerThread::CheckIfNameHasChanged(int chair)
{
	char		oh_scraped_name[k_max_length_of_playername]; 
	char		best_name[k_max_length_of_playername];

	assert(chair >= k_first_chair); 
	assert(chair <= k_last_chair);
	
	memset(oh_scraped_name, 0, k_max_length_of_playername);
	memset(best_name, 0, k_max_length_of_playername);
	
	if (p_game_state->state((p_game_state->state_index()-1)&0xff)->m_player[chair].m_name_known == 0)
	{
		return false;
	}
	strcpy_s(oh_scraped_name, k_max_length_of_playername, p_game_state->state((p_game_state->state_index()-1)&0xff)->m_player[chair].m_name);

	if (NameLooksLikeBadScrape(oh_scraped_name))
	{
		return false;
	}

	// We already have the name, and it has not changed since we last checked, so do nothing
	if (_player_data[chair].found 
		&& 0 == strcmp(_player_data[chair].scraped_name, oh_scraped_name))
	{
		return false;
	}
	write_log(preferences.debug_pokertracker(), "[PokerTracker] Name changed for chair [%d] [%s] -> [%s]\n",
		chair, _player_data[chair].scraped_name, oh_scraped_name);
	return true;
}


void CPokerTrackerThread::SetPlayerName(int chr, bool found, const char* pt_name, const char* scraped_name)
{
	_player_data[chr].found = found;
	bool logResult = false;
	if (0 != memcmp(_player_data[chr].pt_name, pt_name, k_max_length_of_playername) )
	{
		memcpy(_player_data[chr].pt_name, pt_name, k_max_length_of_playername);
		logResult = true;
	}
	if (0 != memcmp(_player_data[chr].scraped_name, scraped_name, k_max_length_of_playername) )
	{
		memcpy(_player_data[chr].scraped_name, scraped_name, k_max_length_of_playername);
		logResult = true;
	}
	if (logResult)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] SetPlayerName[%d]: Done. ptname[%s] scrapedName[%s]\n", chr, _player_data[chr].pt_name, _player_data[chr].scraped_name);
	}
}


//Short Algorithm to query the DB for the best name. returns true if best name is populated.
bool CPokerTrackerThread::FindName(const char *oh_scraped_name, char *best_name)
{

	bool result = QueryName(oh_scraped_name, oh_scraped_name, best_name);

	if (!result) {
		/*  Escalation #1: Exact name not found in DB*/
		/*  Try query with "%" in substitution of ".". So "n.me" becomes "n%me". */
		char pointlessname[k_max_length_of_playername + 1];
		memset(pointlessname, 0, k_max_length_of_playername+1);
		int len = (int)strlen(oh_scraped_name);
		for (int character_position=0; character_position<len; character_position++)
		{
			if (oh_scraped_name[character_position]=='.') {
				pointlessname[character_position]='%';
				} 
			else {
				pointlessname[character_position]=oh_scraped_name[character_position];
				}
		}
		pointlessname[len]='\0';
		result = QueryName(pointlessname, oh_scraped_name, best_name);
	}

	if (!result) {
		/*  Escalation #2:
		/*  Try query with "%n%a%m%e%" . */
		char likename[2 * k_max_length_of_playername + 2];
		// Attention: likename is 2 times as large as a player-name,
		// plus 2 for % at the beginning and \0 at the end.
		// !! There was a buffer-overflow in the past; better get rid of fixed-sized buffers
		memset(likename, 0, 2 * k_max_length_of_playername + 2);
		int len = (int)strlen(oh_scraped_name);
		likename[0]='%';
		for (int character_position=0; character_position<len; character_position++)
		{
			likename[character_position*2+1]=oh_scraped_name[character_position];
			likename[character_position*2+2]='%';
		}

		likename[len*2 + 1]='\0';
		result = QueryName(likename, oh_scraped_name, best_name);
	}

	if (!result)
	{
		/*  Escalation #3: "%n%a%m%e%" not found in DB*/
		/*  Try query "%" to get all names. */
		result = QueryName("%", oh_scraped_name, best_name);
	}
	return result;
}

double CPokerTrackerThread::UpdateStat(int m_chr, int stat)
{
	PGresult	*res = NULL;
	double		result = k_undefined;
	clock_t		updStart, updEnd;
	int			  duration;

	int sym_elapsed = p_symbol_engine_time->elapsed();

	//No more unnecessary queries when we don't even have a siteid to check
	int siteid = pt_lookup.GetSiteId();
	if (siteid == k_undefined)
		return k_undefined;

	if (!_connected || PQstatus(_pgconn) != CONNECTION_OK)
		return k_undefined;

	assert(m_chr >= k_first_chair);
	assert(m_chr <= k_last_chair);
	assert(stat >= 0);
	assert(stat < PT_DLL_GetNumberOfStats());

	// get query string for the requested statistic
	CString query = PT_DLL_GetQuery(stat,
		p_symbol_engine_isomaha->isomaha(),
		p_symbol_engine_istournament->istournament(),
		siteid, _player_data[m_chr].pt_name);

	// Do the query against the PT database
	updStart = clock();
	try
	{
		// See if we can find the player name in the database
		write_log(preferences.debug_pokertracker(), 
			"[PokerTracker] Querying %s for m_chr %d: %s\n", 
			PT_DLL_GetBasicSymbolNameWithoutPTPrefix(stat), 
			m_chr, query);
		res = PQexec(_pgconn, query);
	}
	catch (_com_error &e)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] ERROR\n");
		write_log(preferences.debug_pokertracker(), _T("\tCode = %08lx\n"), e.Error());
		write_log(preferences.debug_pokertracker(), _T("\tCode meaning = %s\n"), e.ErrorMessage());
		_bstr_t bstrSource(e.Source());
		_bstr_t bstrDescription(e.Description());
		write_log(preferences.debug_pokertracker(), _T("\tSource = %s\n"), (LPCTSTR) bstrSource);
		write_log(preferences.debug_pokertracker(), _T("\tDescription = %s\n"), (LPCTSTR) bstrDescription);
		write_log(preferences.debug_pokertracker(), _T("\tQuery = [%s]\n"), query);
	}
	
	updEnd = clock();
	duration = (int) ((double)(updEnd - updStart) / 1000);
	if (duration >= 3)
		write_log(preferences.debug_pokertracker(), "[PokerTracker] Query time in seconds: [%d]\n", duration);

	// Check query return code
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		switch (PQresultStatus(res))
		{
		case PGRES_COMMAND_OK:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_COMMAND_OK: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_EMPTY_QUERY:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_EMPTY_QUERY: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_BAD_RESPONSE:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_BAD_RESPONSE: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_COPY_OUT:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_COPY_OUT: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_COPY_IN:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_COPY_IN: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_NONFATAL_ERROR:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_NONFATAL_ERROR: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		case PGRES_FATAL_ERROR:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] PGRES_FATAL_ERROR: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		default:
			write_log(preferences.debug_pokertracker(), "[PokerTracker] GENERIC ERROR: %s [%s]\n", PQerrorMessage(_pgconn), query);
			break;
		}
	}
	else
	{
		if (PQgetisnull(res,0,0) != 1)
		{
			result = atof(PQgetvalue(res,0,0));
			write_log(preferences.debug_pokertracker(), 
				"[PokerTracker] Query %s for m_chr %d success: %f\n", 
				PT_DLL_GetBasicSymbolNameWithoutPTPrefix(stat), 
				m_chr, result);
		}
		PQclear(res);

		// update cache with new values
		PT_DLL_SetStat(stat, m_chr, result);
	}

	return result;
}

bool CPokerTrackerThread::QueryName(const char * query_name, const char * scraped_name, char * best_name)
{
	int				lev_dist = 0, bestLD = 0, bestLDindex = 0;
	PGresult		*res = NULL;
	bool			result = false;
	CLevDistance	myLD;
	int				siteid = 0;
	double			Levenshtein_distance_matching_factor = 0.3;

	//No more unnecessary queries when we don't even have a siteid to check
	siteid = pt_lookup.GetSiteId();
	if (siteid == k_undefined)  return false;

	if (!_connected || PQstatus(_pgconn) != CONNECTION_OK)
		return false;

	if (0 == strlen(query_name))
		return false;

	CString query;
	query.Format("SELECT player_name FROM player WHERE player_name like '%s' AND id_site=%i",
		query_name, siteid);
	try
	{
		res = PQexec(_pgconn, query);
	}
	catch (_com_error &e)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] Postgres Query error:\n");
		write_log(preferences.debug_pokertracker(), "[PokerTracker] \tCode = %08lx\n", e.Error());
		write_log(preferences.debug_pokertracker(), "[PokerTracker] \tCode meaning = %s\n", e.ErrorMessage());
		_bstr_t bstrSource(e.Source());
		_bstr_t bstrDescription(e.Description());
		write_log(preferences.debug_pokertracker(), "[PokerTracker] \tSource = %s\n", (LPCTSTR) bstrSource);
		write_log(preferences.debug_pokertracker(), "[PokerTracker] \tDescription = %s\n", (LPCTSTR) bstrDescription);
		write_log(preferences.debug_pokertracker(), "[PokerTracker] \tQuery = [%s]\n", query);
	}

	// We got nothing, return false
	if (PQntuples(res) == 0)
	{
		result = false;
	}

	// If we get one tuple, we still check for Levenshtein distance if the name is short.
	// If the name is long it is probably displayed truncated so Levenshtein distance would be too high.
	if (PQntuples(res) == 1)
	{
		char *found_name = PQgetvalue(res, 0, 0);
		lev_dist = myLD.LD(scraped_name, found_name);
		if (	strlen(found_name) >= k_min_name_length_to_skip_lev_dist
			 || lev_dist <= (int)strlen(found_name) * Levenshtein_distance_matching_factor )
		{
			strncpy_s(best_name, k_max_length_of_playername, found_name, _TRUNCATE);
			// Max length names are possibly truncated so we replace the last character by %.
			if (strlen(best_name)==(k_max_length_of_playername-1)) {
				best_name[(k_max_length_of_playername-2)] = '%';
			}
			result = true;
		}
		else {
			result = false;
		}
	}

	// We got more than one tuple, figure the Levenshtein distance for all of them
	// and return the best
	else if (PQntuples(res) > 1)
	{
		bestLD = 999;
		for (int i=0; i<PQntuples(res); i++)
		{
			lev_dist = myLD.LD(scraped_name, PQgetvalue(res, i, 0));

			if (lev_dist<bestLD && lev_dist<(int)strlen(PQgetvalue(res, i, 0))*Levenshtein_distance_matching_factor ) 
			{
				bestLD = lev_dist;
				bestLDindex = i;
			}
		}
		if (bestLD != 999)
		{
			strncpy_s(best_name, k_max_length_of_playername, PQgetvalue(res, bestLDindex, 0), _TRUNCATE);
			// Max length names are possibly truncated so we replace the last character by %.
			if (strlen(best_name)==(k_max_length_of_playername-1)) {
				best_name[(k_max_length_of_playername-2)] = '%';
			}
			result = true;
		}
		else
		{
			result = false;
		}
	}

	PQclear(res);
	return result;
}


bool CPokerTrackerThread::UpdateAllStats(int chr)
{
	if (_player_data[chr].skipped_updates == k_advanced_stat_update_every)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] UpdateAllStats: for chair [%d] is true.\n", chr);
		return true;
	}
	write_log(preferences.debug_pokertracker(), "[PokerTracker] UpdateAllStats: for chair [%d] is false (Basic only).\n", chr);
	return false;
}

void CPokerTrackerThread::RecalcSkippedUpdates(int chr)
{
	if (_player_data[chr].skipped_updates == k_advanced_stat_update_every)
		_player_data[chr].skipped_updates = 1;
	else
		++_player_data[chr].skipped_updates;
}


int CPokerTrackerThread::SkipUpdateCondition(int stat, int chair)
{
	if (UpdateAllStats(chair))
	{
		return false;
	}
	else if (PT_DLL_IsPositionalPreflopStat(stat) 
		|| PT_DLL_IsAdvancedStat(stat))
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool CPokerTrackerThread::SkipUpdateForChair(int chair)
{
	int userchair = p_symbol_engine_userchair->userchair();
	bool confirmed = p_symbol_engine_userchair->userchair_confirmed();
	if (userchair == chair && confirmed)
	{
		write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair for chair [%d] had been skipped. Reason: [User sits in this chair]\n", chair);
		return true;
	}
	
	int hands = (int)PT_DLL_GetStat("pt_hands", chair);
	if (hands > k_min_hands_for_slower_updates)
	{
		if (UpdateAllStats(chair))
			return false;
		else
		{
			write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair for chair [%d] had been skipped. Reason: [User has lots of hands]\n", chair);
			return true;
		}
	}
	return false;
}


void CPokerTrackerThread::GetStatsForChair(LPVOID pParam, int chair, int sleepTime)
{
	CPokerTrackerThread *pParent = static_cast<CPokerTrackerThread*>(pParam);
	int         updatedCount = 0;

  write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair %i\n", chair);  
	if (pParent->CheckIfNameExistsInDB(chair) == false)
	{
		/* Note that checkname fail just when starting, doesn't necessarily mean that there's no user
		   in that chair, but only that the scraper failed to find one. This could be due to lobby window
		   that hides poker window behind it. We make this check once, and if we are good, the update iteration
		   is good to go. if we are not, we assume that this seat is not taken at the moment. */ 
		write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair[%d] had been skipped. Reason: [CheckName failed]\n", chair);
		return;
	}
	write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair[%d] had been started.\n", chair);
	/* Check if there's a complete update cycle skipping for that chair */
	if (pParent->SkipUpdateForChair(chair))
	{
		pParent->RecalcSkippedUpdates(chair);
		return;
	}

	if (!pParent->_connected)
	{
		pParent->Connect();
	}
		
	if (pParent->_connected && PQstatus(pParent->_pgconn) == CONNECTION_OK)
	{
		if (p_autoconnector->IsConnected())
		{
			for (int i=0; i<PT_DLL_GetNumberOfStats(); i++)
			{
				/* CheckName is necessary before each update.
				   There's a short interval between any two updates, and it's possible that the player
				   had stood up during the update process. But it also possible that the poker lobby was 
				   hiding our poker window, or some popup temporarily was over it, and that's why CheckName fails.
				   Since we cannot know which one caused checkname to fail, we would continue to update, as 
				   long as we have a found name, and as long as the name did't get changed. 
				   So what we do care about, is the situation were the name got replaced by another name,
				   in that case, we stop the update for the current chair  			   */ 

				if (_player_data[chair].found)
				{
					/* Verify therad_stop is false */ 
					if (LightSleep(0, pParent)) 
						return; 
					/* verify that name did not get changed */
					if (pParent->CheckIfNameHasChanged(chair))
					{
						/* Name got changed while we search for stats for current chair
						   Simply return.
						   Clearing stats happens by CSymbolEnginePokerTracker
						   on next symbol lookup   .
						*/
						write_log(preferences.debug_pokertracker(), "[PokerTracker] Name changed for chair [%d] Stopping PT-symbol-lookup. \n", chair);
						return;
					}
					if (pParent->SkipUpdateCondition(i, chair))
					{
						/* Updating stat i should be skipped this time */
						/* advanced/positional stats are updated every k_advanced_stat_update_every cycles */
						write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair: Updating stats [%d] for chair [%d] had been skipped. Reason: [advanced/positional stats cycle [%d of %d]]\n", i, chair, pParent->GetSkippedUpdates(chair) , k_advanced_stat_update_every);
					}
					else
					{
						/* Update... */
						write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair updating stats [%d] for chair [%d]...\n", i, chair);
						pParent->UpdateStat(chair, i);
						++updatedCount;
					}
					/* Sleep between two updates (even if skipped) */
					if (LightSleep(sleepTime, pParent)) 
						return;
				}
				else
				{
					/* We couldn't find any user sitting on that chair. Give message*/
					write_log(preferences.debug_pokertracker(), "[PokerTracker] GetStatsForChair for chair [%d] had been skipped. Reason: [user not found (user stood up?)]\n", chair);
					return;
				}
			}
		}
	}
	pParent->ReportUpdateComplete(updatedCount, chair);
	pParent->RecalcSkippedUpdates(chair);
}

void CPokerTrackerThread::ReportUpdateComplete(int updatedCount, int chair)
{
	write_log(preferences.debug_pokertracker(), "[PokerTracker] Updates for chair [%d][%s] had been completed. Total [%d] updated stats\n", chair, _player_data[chair].scraped_name, updatedCount);
}

UINT CPokerTrackerThread::PokertrackerThreadFunction(LPVOID pParam)
{
	CPokerTrackerThread *pParent = static_cast<CPokerTrackerThread*>(pParam);
	int				iteration = 0;
	clock_t		iterStart, iterEnd;
	int				iterDurationMS;

	while (::WaitForSingleObject(pParent->_m_stop_thread, 0) != WAIT_OBJECT_0)
	{
		iterStart = clock();
		write_log(preferences.debug_pokertracker(), "[PokerTracker] PTthread iteration [%d] had started\n", ++iteration);
		if (!pParent->_connected)
		{
			pParent->Connect();
		}
	
		double players = p_symbol_engine_active_dealt_playing->nopponentsplaying() 
			+ (p_symbol_engine_userchair->userchair_confirmed() ? 1 : 0); 
		write_log(preferences.debug_pokertracker(), "[PokerTracker] Players count is [%d]\n", players);
				
		if (players < 2)
		{
			write_log(preferences.debug_pokertracker(), "[PokerTracker] Not enough players to justify iteration...\n");
			write_log(preferences.debug_pokertracker(), "[PokerTracker] For beginners: possible tablemap-problem?\n");
			write_log(preferences.debug_pokertracker(), "[PokerTracker] Continuing anyway...\n");
		}

		// Avoiding division by zero and setting sleep time
		AdaptValueToMinMaxRange(&players, 1, k_max_number_of_players);
		int sleep_time = (int) ((double)(30 * 1000) / (double)((PT_DLL_GetNumberOfStats() + 1) * players));
		write_log(preferences.debug_pokertracker(), "[PokerTracker] sleepTime set to %d\n", sleep_time);
		LightSleep(sleep_time, pParent);
		
		if (pParent->_connected && PQstatus(pParent->_pgconn) == CONNECTION_OK)
		{
			for (int chair = 0; chair < p_tablemap->nchairs(); ++chair)
			{
				GetStatsForChair(pParam, chair, sleep_time);
				/* Verify therad_stop is false */ 
				if (LightSleep(0, pParent)) 
					break; 
			}
		}
		iterEnd = clock();
		iterDurationMS = (int) ((double)(iterEnd - iterStart));
		write_log(preferences.debug_pokertracker(), "[PokerTracker] PTthread iteration [%d] had ended, duration time in ms: [%d]\n", ++iteration, iterDurationMS);
		if ( (iterDurationMS <= 10000) && (iterDurationMS > 0) )
		{
			write_log(preferences.debug_pokertracker(), "[PokerTracker] sleeping [%d] ms because iteration was too quick.\n", 10000 - iterDurationMS);
			if (LightSleep(10000 - iterDurationMS, pParent)) 
				break; 
		}
	}
	// Set event
	write_log(preferences.debug_pokertracker(), "[PokerTracker] PokertrackerThreadFunction: outside while loop...\n");
	::SetEvent(pParent->_m_wait_thread);
	return 0;
}

/*Sleeps but wakes up on stop thread event
We use this function since we never want the thread to ignore the stop_thread event while it's sleeping*/
int	CPokerTrackerThread::LightSleep(int sleepTime, CPokerTrackerThread *pParent)
{
	write_log(preferences.debug_pokertracker(), "[PokerTracker] LightSleep: called with sleepTime[%d]\n", sleepTime);
	if ( sleepTime > 0)
	{
		int iterations = 20;
		int sleepSlice = (int) ((double)sleepTime / (double)iterations);
		for (int i = 0; i < iterations; i++)
		{
			Sleep(sleepSlice);
			if (::WaitForSingleObject(pParent->_m_stop_thread, 0) == WAIT_OBJECT_0)
			{
				write_log(preferences.debug_pokertracker(), "[PokerTracker] LightSleep: _m_stop_thread signal received\n");
				return 1;
			}
		}
	}
	else
	{
		if (::WaitForSingleObject(pParent->_m_stop_thread, 0) == WAIT_OBJECT_0)
		{
			write_log(preferences.debug_pokertracker(), "[PokerTracker] LightSleep: _m_stop_thread signal received\n");
			return 1;
		}
	}
	return 0;
}