/* 
 *
 * mod_nuance_transcribe.c -- Freeswitch module for real-time transcription using nuance's gRPC interface
 *
 */
#include "mod_nuance_transcribe.h"
#include "nuance_glue.h"
#include <stdlib.h>
#include <switch.h>

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_transcribe_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_transcribe_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_nuance_transcribe, mod_transcribe_load, mod_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);


static void responseHandler(switch_core_session_t* session, const char * json, const char* bugname) {
	switch_event_t *event;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (0 == strcmp("vad_detected", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_VAD_DETECTED);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("end_of_utterance", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_END_OF_UTTERANCE);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("end_of_transcript", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("start_of_transcript", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_START_OF_TRANSCRIPT);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("max_duration_exceeded", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("no_audio", json)) {
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else if (0 == strcmp("play_interrupt", json)){
		switch_event_t *qevent;
		switch_status_t status;
		if (switch_event_create(&qevent, SWITCH_EVENT_DETECTED_SPEECH) == SWITCH_STATUS_SUCCESS) {
			if ((status = switch_core_session_queue_event(session, &qevent)) != SWITCH_STATUS_SUCCESS){
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to queue play inturrupt event  %d \n", status);
			}
		}else{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unable to create play inturrupt event \n");
		}
		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_PLAY_INTERRUPT);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
	}
	else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s json payload: %s.\n", bugname ? bugname : "nuance_transcribe", json);

		switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, TRANSCRIBE_EVENT_RESULTS);
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "nuance");
		switch_event_add_body(event, "%s", json);
	}
	if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
	switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_INIT.\n");
			responseHandler(session, "start_of_transcript", cb->bugname);
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE, calling nuance_speech_session_cleanup.\n");
			responseHandler(session, "end_of_transcript", cb->bugname);
			nuance_speech_session_cleanup(session, 1, bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return nuance_speech_frame(bug, user_data);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t do_stop(switch_core_session_t *session, char *bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command command, calling nuance_speech_session_cleanup (possibly to stop prev transcribe)\n");
		status = nuance_speech_session_cleanup(session, 0, bug);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stopped transcription.\n");
	}

	return status;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  char* lang, int interim, char* bugname)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_implementation_t read_impl = { 0 };
	void *pUserData;
	uint32_t samples_per_second;
	int single_utterance = 0, separate_recognition = 0, max_alternatives = 0, profanity_filter = 0, word_time_offset = 0, punctuation = 0, enhanced = 0;
	char* hints = NULL;
  char* model = NULL;

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
		do_stop(session, bugname);
	}

	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_SINGLE_UTTERANCE"))) {
      single_utterance = 1;
    }

	// transcribe each separately?
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_SEPARATE_RECOGNITION_PER_CHANNEL"))) {
      separate_recognition = 1;
    }

	// max alternatives
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_MAX_ALTERNATIVES"))) {
     	max_alternatives = atoi(switch_channel_get_variable(channel, "NUANCE_SPEECH_MAX_ALTERNATIVES"));
    }

	// profanity filter
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_PROFANITY_FILTER"))) {
      profanity_filter = 1;
    }

	// enable word offsets
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_ENABLE_WORD_TIME_OFFSETS"))) {
      word_time_offset = 1;
    }

	// enable automatic punctuation
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_ENABLE_AUTOMATIC_PUNCTUATION"))) {
      punctuation = 1;
    }

    // speech model
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_MODEL"))) {	
		model = (char *)switch_channel_get_variable(channel, "NUANCE_SPEECH_MODEL");    
	}
    

    // use enhanced model
    if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_USE_ENHANCED"))) {
      enhanced = 1;
    }

	// hints
	if (switch_true(switch_channel_get_variable(channel, "NUANCE_SPEECH_HINTS"))) {	
	  hints = (char *)switch_channel_get_variable_dup(channel, "NUANCE_SPEECH_HINTS", SWITCH_TRUE, -1);
	}

	switch_core_session_get_read_impl(session, &read_impl);

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

	if (SWITCH_STATUS_FALSE == nuance_speech_session_init(session, responseHandler, samples_per_second, flags & SMBF_STEREO ? 2 : 1, lang, interim, bugname, single_utterance,
	 separate_recognition, max_alternatives, profanity_filter, word_time_offset, punctuation, model, enhanced, hints, NULL, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing nuance speech session.\n");
		return SWITCH_STATUS_FALSE;
	}

	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	switch_channel_set_private(channel, bugname, bug);

	return SWITCH_STATUS_SUCCESS;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] [lang-code] [interim|full] [stereo|mono] [bug-name]"
SWITCH_STANDARD_API(transcribe_function)
{
	char *mycmd = NULL, *argv[6] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || 
      (!strcasecmp(argv[1], "stop") && argc < 2) ||
      (!strcasecmp(argv[1], "start") && argc < 3) ||
      zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
		stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
				char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing\n");
				status = do_stop(lsession, bugname);
			} else if (!strcasecmp(argv[1], "start")) {
        char* lang = argv[2];
        int interim = argc > 3 && !strcmp(argv[3], "interim");
				char *bugname = argc > 5 ? argv[5] : MY_BUG_NAME;
				if (argc > 4 && !strcmp(argv[4], "stereo")) {
          flags |= SMBF_WRITE_STREAM ;
          flags |= SMBF_STEREO;
				}
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s start transcribing %s %s\n", bugname, lang, interim ? "interim": "complete");
				status = start_capture(lsession, flags, lang, interim, bugname);
			}
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

  done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_transcribe_load)
{
	switch_api_interface_t *api_interface;

	/* create/register custom event message type */
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_RESULTS) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_RESULTS);
		return SWITCH_STATUS_TERM;
	}
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_END_OF_UTTERANCE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_END_OF_UTTERANCE);
		return SWITCH_STATUS_TERM;
	}
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_START_OF_TRANSCRIPT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_START_OF_TRANSCRIPT);
		return SWITCH_STATUS_TERM;
	}
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_END_OF_TRANSCRIPT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
		return SWITCH_STATUS_TERM;
	}
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_NO_AUDIO_DETECTED) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
		return SWITCH_STATUS_TERM;
	}
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
		return SWITCH_STATUS_TERM;
	}

	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_PLAY_INTERRUPT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_PLAY_INTERRUPT);
		return SWITCH_STATUS_TERM;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Nuance Speech Transcription API loading..\n");

  if (SWITCH_STATUS_FALSE == nuance_speech_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing nuance speech interface\n");
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Nuance Speech Transcription API successfully loaded\n");

	SWITCH_ADD_API(api_interface, "uuid_nuance_transcribe", "Nuance Speech Transcription API", transcribe_function, TRANSCRIBE_API_SYNTAX);
	switch_console_set_complete("add uuid_nuance_transcribe start lang-code");
	switch_console_set_complete("add uuid_nuance_transcribe stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_nuance_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_transcribe_shutdown)
{
	nuance_speech_cleanup();
	switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
	switch_event_free_subclass(TRANSCRIBE_EVENT_END_OF_UTTERANCE);
	switch_event_free_subclass(TRANSCRIBE_EVENT_START_OF_TRANSCRIPT);
	switch_event_free_subclass(TRANSCRIBE_EVENT_END_OF_TRANSCRIPT);
	switch_event_free_subclass(TRANSCRIBE_EVENT_NO_AUDIO_DETECTED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_MAX_DURATION_EXCEEDED);
	switch_event_free_subclass(TRANSCRIBE_EVENT_END_OF_UTTERANCE);
	switch_event_free_subclass(TRANSCRIBE_EVENT_PLAY_INTERRUPT);
	return SWITCH_STATUS_SUCCESS;
}

