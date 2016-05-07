/** @file payoutd.c
 *  @brief Main source file for the payoutd daemon.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>

// lowlevel library provided by the cash hardware vendor
// innovative technologies (http://innovative-technology.com).
// ssp manual available at http://innovative-technology.com/images/pdocuments/manuals/SSP_Manual.pdf
#include "port_linux.h"
#include "ssp_defines.h"
#include "ssp_commands.h"
#include "SSPComs.h"

// json library
#include <jansson.h>

// c client for redis
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
// adding libevent adapter for hiredis async
#include <hiredis/adapters/libevent.h>

#include <syslog.h>

// libuuid is used to generate msgIds for the responses
#include <uuid/uuid.h>

// https://sites.google.com/site/rickcreamer/Home/cc/c-implementation-of-stringbuffer-functionality
#include "StringBuffer.h"
#include "StringBuffer.c"

redisAsyncContext *redisPublishCtx = NULL;		// redis context used for publishing messages
redisAsyncContext *redisSubscribeCtx = NULL;	// redis context used for subscribing to topics

struct m_metacash;

/**
 * Structure which describes an actual ITL device.
 */
struct m_device {
	int id;
	char *name;
	unsigned long long key;
	unsigned char channelInhibits;

	SSP_COMMAND sspC;
	SSP6_SETUP_REQUEST_DATA sspSetupReq;
	void (*eventHandlerFn) (struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
};

/**
 * Structure which contains the generic setup and
 * the device structures for our two ITL devices.
 */
struct m_metacash {
	int quit;
	int deviceAvailable;
	char *serialDevice;

	int redisPort;
	char *redisHost;

	struct event_base *eventBase; // libevent
	struct event evPoll; // event for periodically polling the cash hardware
	struct event evCheckQuit; // event for periodically checking to quit

	struct m_device hopper; // smart hopper device
	struct m_device validator; // nv200 + smart payout devices
};

/**
 * Structure which describes an actual command which we
 * received in one of our request topics.
 */
struct m_command {
	json_t *jsonMessage;

	char *command;
	char *correlId;
	char *msgId;
	char *responseTopic;

	struct m_device *device;
	struct m_metacash *metacash;
};

// mcSsp* : ssp helper functions
int mcSspOpenSerialDevice(struct m_metacash *metacash);
void mcSspCloseSerialDevice(struct m_metacash *metacash);
void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId);
void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key, struct m_device *device);
void mcSspPollDevice(struct m_device *device, struct m_metacash *metacash);

// mc_ssp_* : ssp magic values and functions (each of these relate directly to a command specified in the ssp protocol)

/** Magic Constant for the "GET FIRMWARE VERSION" command ID as specified in SSP */
#define SSP_CMD_GET_FIRMWARE_VERSION 0x20
/** Magic Constant for the "GET DATASET VERSION" command ID as specified in SSP */
#define SSP_CMD_GET_DATASET_VERSION 0x21
/** Magic Constant for the "GET ALL LEVELS" command ID as specified in SSP */
#define SSP_CMD_GET_ALL_LEVELS 0x22
/** Magic Constant for the "SET DENOMINATION LEVEL" command ID as specified in SSP */
#define SSP_CMD_SET_DENOMINATION_LEVEL 0x34
/** Magic Constant for the "LAST REJECT NOTE" command ID as specified in SSP */
#define SSP_CMD_LAST_REJECT_NOTE 0x17
/** Magic Constant for the "CONFIGURE BEZEL" command ID as specified in SSP */
#define SSP_CMD_CONFIGURE_BEZEL 0x54
/** Magic Constant for the "SMART EMPTY" command ID as specified in SSP */
#define SSP_CMD_SMART_EMPTY 0x52
/** Magic Constant for the "SET REFILL MODE " command ID as specified in SSP */
#define SSP_CMD_SET_REFILL_MODE 0x30
/** Magic Constant for the "DISPLAY OFF" command ID as specified in SSP */
#define SSP_CMD_DISPLAY_OFF 0x4
/** Magic Constant for the "DISPLAY ON" command ID as specified in SSP */
#define SSP_CMD_DISPLAY_ON 0x3

SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_smart_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r, unsigned char g, unsigned char b, unsigned char non_volatile);
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC, unsigned char *reason);
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_all_levels(SSP_COMMAND *sspC, char **json);
SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, const char *cc);
SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value, const char *cc, const char option);
SSP_RESPONSE_ENUM mc_ssp_channel_security_data(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_firmware_version(SSP_COMMAND *sspC, char *firmwareVersion);
SSP_RESPONSE_ENUM mc_ssp_get_dataset_version(SSP_COMMAND *sspC, char *datasetVersion);

/** Magic Constant for the "route to cashbox" option as specified in SSP */
static const char ROUTE_CASHBOX = 0x01;
/** Magic Constant for the "route to storage" option as specified in SSP */
static const char ROUTE_STORAGE = 0x00;

static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash);
void setup(struct m_metacash *metacash);
void hopperEventHandler(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void validatorEventHandler(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

static const char *CURRENCY = "EUR";

/**
 * set by the signalHandler function and checked in cbCheckQuit.
 */
int receivedSignal = 0;

/**
 * Signal handler
 */
void signalHandler(int signal) {
	receivedSignal = signal;
}

/**
 * Waits for 300ms each time called.
 */
void hardwareWaitTime() {
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 300000000;
	nanosleep(&ts, NULL);
}

/**
 * Create a new redisAsyncContext.
 */
redisAsyncContext* connectRedis(struct m_metacash *metacash) {
	redisAsyncContext *conn = redisAsyncConnect(metacash->redisHost,
			metacash->redisPort);

	if (conn == NULL || conn->err) {
		if (conn) {
			fprintf(stderr, "fatal: Connection error: %s\n", conn->errstr);
		} else {
			fprintf(stderr,
					"fatal: Connection error: can't allocate redis context\n");
		}
	} else {
		// reference the metcash struct in data for use in connect/disconnect callback
		conn->data = metacash;
	}

	return conn;
}

/**
 * Callback function for libEvent triggered "Poll" event.
 */
void cbOnPollEvent(int fd, short event, void *privdata) {
	struct m_metacash *metacash = privdata;
	if (metacash->deviceAvailable == 0) {
		// return immediately if we have no actual hardware to poll
		return;
	}

	mcSspPollDevice(&metacash->hopper, metacash);
	mcSspPollDevice(&metacash->validator, metacash);
}

/**
 * Callback function for libEvent triggered "CheckQuit" event.
 */
void cbOnCheckQuitEvent(int fd, short event, void *privdata) {
	if (receivedSignal != 0) {
		syslog(LOG_NOTICE, "received signal. going to exit event loop.");

		struct m_metacash *metacash = privdata;
		event_base_loopexit(metacash->eventBase, NULL);
		receivedSignal = 0;
	}
}

/**
 * Callback function triggered by an incoming message in the "metacash" topic.
 */
void cbOnMetacashMessage(redisAsyncContext *c, void *r, void *privdata) {
	// empty for now
}

/**
 * Test if the message contains the "cmd":"(command)" property
 */
int isCommand(struct m_command *cmd, const char *command) {
	return ! strcmp(cmd->command, command);
}

/**
 * Helper function to publish a message to the "hopper-event" topic.
 */
int publishHopperEvent(char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", "hopper-event", reply);

	free(reply);

	return 0;
}

/**
 * Helper function to publish a message to the "validator-event" topic.
 */
int publishValidatorEvent(char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", "validator-event", reply);

	free(reply);

	return 0;
}

/**
 * Helper function to publish a message to the given topic.
 */
int replyWith(char *topic, char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", topic, reply);

	free(reply);

	return 0;
}

/**
 * Helper function to publish a reply to a message which was missing a
 * mandatory property (or the property was of the wrong type).
 */
int replyWithPropertyError(struct m_command *cmd, char *name) {
	char *msgId = "unknown";
	if(cmd->msgId) {
		msgId = cmd->msgId;
	}

	char *correlId = "unknown";
	if(cmd->correlId) {
		correlId = cmd->correlId;
	}

	return replyWith(cmd->responseTopic,
			"{\"msgId\":\"%s\",\"correlId\":\"%s\",\"error\":\"Property '%s' missing or of wrong type\"}",
			msgId,
			correlId,
			name);
}

/**
 * Helper function to publish a reply to a message which contains a human readable
 * version of the SSP response.
 */
int replyWithSspResponse(struct m_command *cmd, SSP_RESPONSE_ENUM response) {
	if(response == SSP_RESPONSE_OK) {
		return replyWith(cmd->responseTopic, "{\"msgId\":\"%s\",\"correlId\":\"%s\",\"result\":\"ok\"}",
				cmd->msgId,
				cmd->correlId);
	} else {
		char *errorMsg;

		switch(response) {
			case SSP_RESPONSE_UNKNOWN_COMMAND:
				errorMsg = "unknown command";
				break;
			case SSP_RESPONSE_INCORRECT_PARAMETERS:
				errorMsg = "incorrect parameters";
				break;
			case SSP_RESPONSE_INVALID_PARAMETER:
				errorMsg = "invalid parameter";
				break;
			case SSP_RESPONSE_COMMAND_NOT_PROCESSED:
				errorMsg = "command not processed";
				break;
			case SSP_RESPONSE_SOFTWARE_ERROR:
				errorMsg = "software error";
				break;
			case SSP_RESPONSE_CHECKSUM_ERROR:
				errorMsg = "checksum error";
				break;
			case SSP_RESPONSE_FAILURE:
				errorMsg = "failure";
				break;
			case SSP_RESPONSE_HEADER_FAILURE:
				errorMsg = "header failure";
				break;
			case SSP_RESPONSE_KEY_NOT_SET:
				errorMsg = "key not set";
				break;
			case SSP_RESPONSE_TIMEOUT:
				errorMsg = "timeout";
				break;
			default:
				errorMsg = "unknown";
		}

		return replyWith(cmd->responseTopic, "{\"msgId\":\"%s\",\"correlId\":\"%s\",\"sspError\":\"%s\"}",
				cmd->msgId,
				cmd->correlId,
				errorMsg);
	}
}

/**
 * Handles the "quit" command.
 */
void handleQuit(struct m_command *cmd) {
	receivedSignal = 1;
	replyWithSspResponse(cmd, SSP_RESPONSE_OK); // :D
}

/**
 * Handles the "empty" command.
 */
void handleEmpty(struct m_command *cmd) {
	replyWithSspResponse(cmd, mc_ssp_empty(&cmd->device->sspC));
}

/**
 * Handles the "smart-empty" command.
 */
void handleSmartEmpty(struct m_command *cmd) {
	replyWithSspResponse(cmd, mc_ssp_smart_empty(&cmd->device->sspC));
}

/**
 * Handles the "do-payout" and "test-payout" commands.
 */
void handlePayout(struct m_command *cmd) {
	int payoutOption = 0;

	if (isCommand(cmd, "do-payout")) {
		payoutOption = SSP6_OPTION_BYTE_DO;
	} else {
		payoutOption = SSP6_OPTION_BYTE_TEST;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int amount = json_integer_value(jAmount);

	SSP_RESPONSE_ENUM resp = ssp6_payout(&cmd->device->sspC, amount, CURRENCY,
			payoutOption);

	if (resp == SSP_RESPONSE_COMMAND_NOT_PROCESSED) {
		char *error = NULL;
		switch (cmd->device->sspC.ResponseData[1]) {
		case 0x01:
			error = "not enough value in smart payout";
			break;
		case 0x02:
			error = "can't pay exact amount";
			break;
		case 0x03:
			error = "smart payout busy";
			break;
		case 0x04:
			error = "smart payout disabled";
			break;
		default:
			error = "unknown";
			break;
		}

		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"error\":\"%s\"}", cmd->correlId, error);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * Handles the "do-float" and "test-float" commands.
 */
void handleFloat(struct m_command *cmd) {
	// basically a copy of do/test-payout ...
	int payoutOption = 0;

	if (isCommand(cmd, "do-float")) {
		payoutOption = SSP6_OPTION_BYTE_DO;
	} else {
		payoutOption = SSP6_OPTION_BYTE_TEST;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int amount = json_integer_value(jAmount);

	SSP_RESPONSE_ENUM resp = mc_ssp_float(&cmd->device->sspC, amount, CURRENCY,
			payoutOption);

	if (resp == SSP_RESPONSE_COMMAND_NOT_PROCESSED) {
		char *error = NULL;
		switch (cmd->device->sspC.ResponseData[1]) {
		case 0x01:
			error = "not enough value in smart payout";
			break;
		case 0x02:
			error = "can't pay exact amount";
			break;
		case 0x03:
			error = "smart payout busy";
			break;
		case 0x04:
			error = "smart payout disabled";
			break;
		default:
			error = "unknown";
			break;
		}
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"error\":\"%s\"}",
				cmd->correlId, error);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * Print debug output dbgDisplayInhibits.
 */
void dbgDisplayInhibits(unsigned char inhibits) {
	printf("dbgDisplayInhibits: inhibits are: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d\n",
			(inhibits >> 0) & 1,
			(inhibits >> 1) & 1,
			(inhibits >> 2) & 1,
			(inhibits >> 3) & 1,
			(inhibits >> 4) & 1,
			(inhibits >> 5) & 1,
			(inhibits >> 6) & 1,
			(inhibits >> 7) & 1);
}

/**
 * Handles the "enable-channels" command.
 */
void handleEnableChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	// this will be updated and written back to the device state
	// if the update succeeds
	unsigned char currentChannelInhibits = cmd->device->channelInhibits;
	unsigned char highChannels = 0xFF; // actually not in use

	// 8 channels for now, set the bit to 1 for each requested channel
	if(strstr(channels, "1") != NULL) {
		currentChannelInhibits |= 1 << 0;
	}
	if(strstr(channels, "2") != NULL) {
		currentChannelInhibits |= 1 << 1;
	}
	if(strstr(channels, "3") != NULL) {
		currentChannelInhibits |= 1 << 2;
	}
	if(strstr(channels, "4") != NULL) {
		currentChannelInhibits |= 1 << 3;
	}
	if(strstr(channels, "5") != NULL) {
		currentChannelInhibits |= 1 << 4;
	}
	if(strstr(channels, "6") != NULL) {
		currentChannelInhibits |= 1 << 5;
	}
	if(strstr(channels, "7") != NULL) {
		currentChannelInhibits |= 1 << 6;
	}
	if(strstr(channels, "8") != NULL) {
		currentChannelInhibits |= 1 << 7;
	}

	SSP_RESPONSE_ENUM resp = ssp6_set_inhibits(&cmd->device->sspC, currentChannelInhibits, highChannels);

	if(resp == SSP_RESPONSE_OK) {
		// okay, update the channelInhibits in the device structure with the new state
		cmd->device->channelInhibits = currentChannelInhibits;

		if(0) {
			printf("enable-channels:\n");
			dbgDisplayInhibits(currentChannelInhibits);
		}
	}

	replyWithSspResponse(cmd, resp);
}

/**
 * Handles the "disable-channels" command.
 */
void handleDisableChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	// this will be updated and written back to the device state
	// if the update succeeds
	unsigned char currentChannelInhibits = cmd->device->channelInhibits;
	unsigned char highChannels = 0xFF; // actually not in use

	// 8 channels for now, set the bit to 0 for each requested channel
	if(strstr(channels, "1") != NULL) {
		currentChannelInhibits &= ~(1 << 0);
	}
	if(strstr(channels, "2") != NULL) {
		currentChannelInhibits &= ~(1 << 1);
	}
	if(strstr(channels, "3") != NULL) {
		currentChannelInhibits &= ~(1 << 2);
	}
	if(strstr(channels, "4") != NULL) {
		currentChannelInhibits &= ~(1 << 3);
	}
	if(strstr(channels, "5") != NULL) {
		currentChannelInhibits &= ~(1 << 4);
	}
	if(strstr(channels, "6") != NULL) {
		currentChannelInhibits &= ~(1 << 5);
	}
	if(strstr(channels, "7") != NULL) {
		currentChannelInhibits &= ~(1 << 6);
	}
	if(strstr(channels, "8") != NULL) {
		currentChannelInhibits &= ~(1 << 7);
	}

	SSP_RESPONSE_ENUM resp = ssp6_set_inhibits(&cmd->device->sspC, currentChannelInhibits, highChannels);

	if(resp == SSP_RESPONSE_OK) {
		// okay, update the channelInhibits in the device structure with the new state
		cmd->device->channelInhibits = currentChannelInhibits;

		if(0) {
			printf("disable-channels:\n");
			dbgDisplayInhibits(currentChannelInhibits);
		}
	}

	replyWithSspResponse(cmd, resp);
}

/**
 * Handles the "inhibit-channels" command.
 */
void handleInhibitChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	unsigned char lowChannels = 0xFF;
	unsigned char highChannels = 0xFF;

	// 8 channels for now
	if(strstr(channels, "1") != NULL) {
		lowChannels &= ~(1 << 0);
	}
	if(strstr(channels, "2") != NULL) {
		lowChannels &= ~(1 << 1);
	}
	if(strstr(channels, "3") != NULL) {
		lowChannels &= ~(1 << 2);
	}
	if(strstr(channels, "4") != NULL) {
		lowChannels &= ~(1 << 3);
	}
	if(strstr(channels, "5") != NULL) {
		lowChannels &= ~(1 << 4);
	}
	if(strstr(channels, "6") != NULL) {
		lowChannels &= ~(1 << 5);
	}
	if(strstr(channels, "7") != NULL) {
		lowChannels &= ~(1 << 6);
	}
	if(strstr(channels, "8") != NULL) {
		lowChannels &= ~(1 << 7);
	}

	replyWithSspResponse(cmd, ssp6_set_inhibits(&cmd->device->sspC, lowChannels, highChannels));
}

/**
 * Handles the "enable" command.
 */
void handleEnable(struct m_command *cmd) {
	replyWithSspResponse(cmd, ssp6_enable(&cmd->device->sspC));
}

/**
 * Handles the "disable" command.
 */
void handleDisable(struct m_command *cmd) {
	replyWithSspResponse(cmd, ssp6_disable(&cmd->device->sspC));
}

/**
 * Handles the "set-denomination-levels" command.
 */
void handleSetDenominationLevels(struct m_command *cmd) {
	json_t *jLevel = json_object_get(cmd->jsonMessage, "level");
	if(! json_is_integer(jLevel)) {
		replyWithPropertyError(cmd, "level");
		return;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int amount = json_integer_value(jLevel);
	int level = json_integer_value(jAmount);

	if(level > 0) {
		/* Quote from the spec -.-
		 *
		 * A command to increment the level of coins of a denomination stored in the hopper.
		 * The command is formatted with the command byte first, amount of coins to *add*
		 * as a 2-byte little endian, the value of coin as 2-byte little endian and
		 * (if using protocol version 6) the country code of the coin as 3 byte ASCII. The level of coins for a
		 * denomination can be set to zero by sending a zero level for that value.
		 *
		 * In a nutshell: This command behaves only with a level of 0 as expected (setting the absolute value),
		 * otherwise it works like the not existing "increment denomination level" command.
		 */

		// ignore the result for now. we could not do much anyway now.
		mc_ssp_set_denomination_level(&cmd->device->sspC, amount, 0, CURRENCY);
	}

	replyWithSspResponse(cmd, mc_ssp_set_denomination_level(&cmd->device->sspC, amount, level, CURRENCY));
}

/**
 * Handles the "get-all-levels" command.
 */
void handleGetAllLevels(struct m_command *cmd) {
	char *json = NULL;

	SSP_RESPONSE_ENUM resp = mc_ssp_get_all_levels(&cmd->device->sspC, &json);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"levels\":[%s]}", cmd->correlId, json);
	} else {
		replyWithSspResponse(cmd, resp);
	}

	free(json);
}

/**
 * Handles the "get-firmware-version" command.
 */
void handleGetFirmwareVersion(struct m_command *cmd) {
	char firmwareVersion[100] = { 0 };

	SSP_RESPONSE_ENUM resp = mc_ssp_get_firmware_version(&cmd->device->sspC, &firmwareVersion[0]);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"version\":\"%s\"]}", cmd->correlId, firmwareVersion);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * Handles the "get-dataset-version" command.
 */
void handleGetDatasetVersion(struct m_command *cmd) {
	char datasetVersion[100] = { 0 };

	SSP_RESPONSE_ENUM resp = mc_ssp_get_dataset_version(&cmd->device->sspC, &datasetVersion[0]);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"version\":\"%s\"]}",
				cmd->correlId, datasetVersion);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * Handles the "last-reject-note" command.
 */
void handleLastRejectNote(struct m_command *cmd) {
	unsigned char reasonCode;

	SSP_RESPONSE_ENUM resp = mc_ssp_last_reject_note(&cmd->device->sspC, &reasonCode);

	if (resp == SSP_RESPONSE_OK) {
		char *reason = NULL;

		switch (reasonCode) {
		case 0x00: // Note accepted
			reason = "note accepted";
			break;
		case 0x01: // Note length incorrect
			reason = "note length incorrect";
			break;
		case 0x02: // Reject reason 2
			reason = "undisclosed (reject reason 2)";
			break;
		case 0x03: // Reject reason 3
			reason = "undisclosed (reject reason 3)";
			break;
		case 0x04: // Reject reason 4
			reason = "undisclosed (reject reason 4)";
			break;
		case 0x05: // Reject reason 5
			reason = "undisclosed (reject reason 5)";
			break;
		case 0x06: // Channel inhibited
			reason = "channel inhibited";
			break;
		case 0x07: // Second note inserted
			reason = "second note inserted";
			break;
		case 0x08: // Reject reason 8
			reason = "undisclosed (reject reason 8)";
			break;
		case 0x09: // Note recognised in more than one channel
			reason = "note recognised in more than one channel";
			break;
		case 0x0A: // Reject reason 10
			reason = "undisclosed (reject reason 10)";
			break;
		case 0x0B: // Note too long
			reason = "note too long";
			break;
		case 0x0C: // Reject reason 12
			reason = "undisclosed (reject reason 12)";
			break;
		case 0x0D: // Mechanism slow/stalled
			reason = "mechanism slow/stalled";
			break;
		case 0x0E: // Strimming attempt detected
			reason = "strimming attempt detected";
			break;
		case 0x0F: // Fraud channel reject
			reason = "fraud channel reject";
			break;
		case 0x10: // No notes inserted
			reason = "no notes inserted";
			break;
		case 0x11: // Peak detect fail
			reason = "peak detect fail";
			break;
		case 0x12: // Twisted note detected
			reason = "twisted note detected";
			break;
		case 0x13: // Escrow time-out
			reason = "escrow time-out";
			break;
		case 0x14: // Bar code scan fail
			reason = "bar code scan fail";
			break;
		case 0x15: // Rear sensor 2 fail
			reason = "rear sensor 2 fail";
			break;
		case 0x16: // Slot fail 1
			reason = "slot fail 1";
			break;
		case 0x17: // Slot fail 2
			reason = "slot fail 2";
			break;
		case 0x18: // Lens over-sample
			reason = "lens over-sample";
			break;
		case 0x19: // Width detect fail
			reason = "width detect fail";
			break;
		case 0x1A: // Short note detected
			reason = "short note detected";
			break;
		case 0x1B: // Note payout
			reason = "note payout";
			break;
		case 0x1C: // Unable to stack note
			reason = "unable to stack note";
			break;
		default: // not defined in API doc
			reason = "undefined in API";
			break;
		}

		replyWith(cmd->responseTopic,
				"{\"correlId\":\"%s\",\"reason\":\"%s\",\"code\":%ld}",
				cmd->correlId, reason, reasonCode);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * Handles the "channel-security" command.
 */
void handleChannelSecurityData(struct m_command *cmd) {
	mc_ssp_channel_security_data(&cmd->device->sspC);
}

/*
 * Handles the "test" command
 */
void handleTest(struct m_command *cmd) {
	replyWithSspResponse(cmd, SSP_RESPONSE_OK);
}

/**
 * Callback function triggered by an incoming message in either
 * the "hopper-request" or "validator-request" topic.
 */
void cbOnRequestMessage(redisAsyncContext *c, void *r, void *privdata) {
	if (r == NULL) {
		return;
	}

	hardwareWaitTime();

	struct m_metacash *m = c->data;
	redisReply *reply = r;

	// example from http://stackoverflow.com/questions/16213676/hiredis-waiting-for-message
	if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
		if (strcmp(reply->element[0]->str, "subscribe") != 0) {
			char *topic = reply->element[1]->str;
			struct m_command cmd;

			cmd.msgId = NULL;
			cmd.correlId = NULL;
			cmd.command = NULL;

			// decide to which topic the response should be sent to
			if (strcmp(topic, "validator-request") == 0) {
				cmd.device = &m->validator;
				cmd.responseTopic = "validator-response";
			} else if (strcmp(topic, "hopper-request") == 0) {
				cmd.device = &m->hopper;
				cmd.responseTopic = "hopper-response";
			} else {
				printf("error: cbOnRequestMessage subscribed for a topic we don't have a response topic\n");
				return;
			}

			// generate a new 'msgId' for the response itself
			uuid_t uuid;
			uuid_generate_time_safe(uuid);
			char msgId[37] = { 0 }; // ex. "1b4e28ba-2fa1-11d2-883f-0016d3cca427" + "\0"
			uuid_unparse_lower(uuid, msgId);
			cmd.msgId = (char *) &msgId; // points to a local variable. only valid until this block is exited!

			char *message = reply->element[2]->str;

			// try to parse the message as json
			json_error_t error;
			cmd.jsonMessage = json_loads(message, 0, &error);

			if(! cmd.jsonMessage) {
				replyWith(cmd.responseTopic,
						"{\"error\":\"could not parse json\",\"reason\":\"%s\",\"line\":%d}",
						error.text, error.line);
				// no need to json_decref(cmd.jsonMessage) here
				return;
			}

			// extract the 'msgId' property (used as the 'correlId' in a response)
			// this will be the 'correlId' used in replies.
			json_t *jMsgId = json_object_get(cmd.jsonMessage, "msgId");
			if(! json_is_string(jMsgId)) {
				replyWithPropertyError(&cmd, "msgId");
				json_decref(cmd.jsonMessage);
				return;
			} else {
				cmd.correlId = (char *) json_string_value(jMsgId); // cast for now
			}

			// extract the 'cmd' property
			json_t *jCmd = json_object_get(cmd.jsonMessage, "cmd");
			if(! json_is_string(jCmd)) {
				replyWithPropertyError(&cmd, "cmd");
				json_decref(cmd.jsonMessage);
				return;
			} else {
				cmd.command = (char *) json_string_value(jCmd); // cast for now
			}

			// proper json structure, properties cmd and msgId have been verified here.
			// also we know which device is used and where we should send our response to.
			// finally try to dispatch the message to the appropriate command handler
			// function if any. in case we don't know that command we respond with a
			// generic error response.

			printf("processing cmd='%s' from msgId='%s' in topic='%s' for device='%s'\n",
					cmd.command, cmd.correlId, topic, cmd.device->name);

			if(isCommand(&cmd, "quit")) {
				handleQuit(&cmd);
			} else if(isCommand(&cmd, "test")) {
				handleTest(&cmd);
			} else {
				// commands in here need the actual hardware

				if(! m->deviceAvailable) {
					// TODO: an unknown command without the actual hardware will also receive this response :-/
					printf("rejecting cmd='%s' from msgId='%s', hardware unavailable!\n", cmd.command, cmd.correlId);
					replyWith(cmd.responseTopic, "{\"correlId\":\"%s\",\"error\":\"hardware unavailable\"}", cmd.correlId);
				} else {
					if(isCommand(&cmd, "empty")) {
						handleEmpty(&cmd);
					} else if (isCommand(&cmd, "smart-empty")) {
						handleSmartEmpty(&cmd);
					} else if (isCommand(&cmd, "enable")) {
						handleEnable(&cmd);
					} else if (isCommand(&cmd, "disable")) {
						handleDisable(&cmd);
					} else if(isCommand(&cmd, "enable-channels")) {
						handleEnableChannels(&cmd);
					} else if(isCommand(&cmd, "disable-channels")) {
						handleDisableChannels(&cmd);
					} else if(isCommand(&cmd, "inhibit-channels")) {
						handleInhibitChannels(&cmd);
					} else if (isCommand(&cmd, "test-float") || isCommand(&cmd, "do-float")) {
						handleFloat(&cmd);
					} else if (isCommand(&cmd, "test-payout") || isCommand(&cmd, "do-payout")) {
						handlePayout(&cmd);
					} else if (isCommand(&cmd, "get-firmware-version")) {
						handleGetFirmwareVersion(&cmd);
					} else if (isCommand(&cmd, "get-dataset-version")) {
						handleGetDatasetVersion(&cmd);
					} else if (isCommand(&cmd, "channel-security-data")) {
						handleChannelSecurityData(&cmd);
					} else if (isCommand(&cmd, "get-all-levels")) {
						handleGetAllLevels(&cmd);
					} else if (isCommand(&cmd, "set-denomination-level")) {
						handleSetDenominationLevels(&cmd);
					} else if (isCommand(&cmd, "last-reject-note")) {
						handleLastRejectNote(&cmd);
					} else {
						replyWith(cmd.responseTopic, "{\"correlId\":\"%s\",\"error\":\"unknown command\",\"cmd\":\"%s\"}",
								cmd.correlId, cmd.command);
					}
				}
			}

			// this will also free the other json objects associated with it
			json_decref(cmd.jsonMessage);
		}
	}
}

/**
 * Callback function triggered by the redis client on connecting with
 * the "publish" context.
 */
void cbOnConnectPublishContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "cbOnConnectPublishContext: redis error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "cbOnConnectPublishContext: connected to redis\n");
}

/**
 * Callback function triggered by the redis client on disconnecting with
 * the "publish" context.
 */
void cbOnDisconnectPublishContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "cbOnDisconnectPublishContext: redis error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "cbOnDisconnectPublishContext: disconnected from redis\n");
}

/**
 * Callback function triggered by the redis client on connecting with
 * the "subscribe" context.
 */
void cbOnConnectSubscribeContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "cbOnConnectSubscribeContext - redis error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "cbOnConnectSubscribeContext - connected to redis\n");

	redisAsyncContext *cNotConst = (redisAsyncContext*) c; // get rids of discarding qualifier \"const\" warning

	// subscribe the topics in redis from which we want to receive messages
	redisAsyncCommand(cNotConst, cbOnMetacashMessage, NULL, "SUBSCRIBE metacash");

	// n.b: the same callback function handles both topics
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL, "SUBSCRIBE validator-request");
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL, "SUBSCRIBE hopper-request");
}

/**
 * Callback function triggered by the redis client on disconnecting with
 * the "subscribe" context.
 */
void cbOnDisconnectSubscribeContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "cbOnDisconnectSubscribeContext - redis error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "cbOnDisconnectSubscribeContext - disconnected from redis\n");
}

/**
 * Entry point of the daemon.
 */
int main(int argc, char *argv[]) {
	// setup logging via syslog
	setlogmask(LOG_UPTO(LOG_NOTICE));
	openlog("payoutd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	syslog(LOG_NOTICE, "Program started by User %d", getuid());

	// register interrupt handler for signals
	signal(SIGTERM, signalHandler);
	signal(SIGINT, signalHandler);

	struct m_metacash metacash;
	metacash.deviceAvailable = 0;
	metacash.quit = 0;

	metacash.serialDevice = "/dev/ttyACM0";	// default, override with -d argument
	metacash.redisHost = "127.0.0.1";	// default, override with -h argument
	metacash.redisPort = 6379;			// default, override with -p argument

	metacash.hopper.id = 0x10; // 0X10 -> Smart Hopper ("Münzer")
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.eventHandlerFn = hopperEventHandler;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.eventHandlerFn = validatorEventHandler;

	// parse the command line arguments
	if (parseCmdLine(argc, argv, &metacash)) {
		return 1;
	}

	syslog(LOG_NOTICE, "using redis at %s:%d and hardware device %s",
			metacash.redisHost, metacash.redisPort, metacash.serialDevice);

	// open the serial device
	if (mcSspOpenSerialDevice(&metacash) == 0) {
		metacash.deviceAvailable = 1;
	} else {
		syslog(LOG_ALERT, "cash hardware unavailable");
	}

	// setup the ssp commands, configure and initialize the hardware
	setup(&metacash);

	syslog(LOG_NOTICE, "metacash open for business :D");

	event_base_dispatch(metacash.eventBase); // blocking until exited via api-call

	syslog(LOG_NOTICE, "exiting");

	if (metacash.deviceAvailable) {
		mcSspCloseSerialDevice(&metacash);
	}

	// cleanup stuff before exiting.

	// redis
	redisAsyncFree(redisPublishCtx);
	redisAsyncFree(redisSubscribeCtx);

	// libevent
	event_base_free(metacash.eventBase);

	// syslog
	closelog();

	return 0;
}

/**
 * Parse the command line arguments.
 */
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash) {
	opterr = 0;

	char c;
	while ((c = getopt(argc, argv, "h:p:d:")) != -1) {
		switch (c) {
		case 'h':
			metacash->redisHost = optarg;
			break;
		case 'p':
			metacash->redisPort = atoi(optarg);
			break;
		case 'd':
			metacash->serialDevice = optarg;
			break;
		case '?':
			if (optopt == 'h' || optopt == 'p' || optopt == 'd') {
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			} else if (isprint(optopt)) {
				fprintf(stderr, "Unknown option '-%c'.\n", optopt);
			} else {
				fprintf(stderr, "Unknown option character 'x%x'.\n", optopt);
			}
			return 1;
		default:
			return 1;
		}
	}

	return 0;
}

/**
 *  Callback function used for publishing events reported by the Hopper hardware.
 */
void hopperEventHandler(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {

	int i;
	for (i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishHopperEvent("{\"event\":\"unit reset\"}");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the \"read\" event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				publishHopperEvent("{\"event\":\"read\",\"channel\":%ld}", poll->events[i].data1);
			} else {
				// this is reported more than once for a single note
				publishHopperEvent("{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_DISPENSING:
			publishHopperEvent("{\"event\":\"dispensing\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_DISPENSED:
			publishHopperEvent("{\"event\":\"dispensed\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_FLOATING:
			publishHopperEvent("{\"event\":\"floating\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_FLOATED:
			publishHopperEvent("{\"event\":\"floated\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CASHBOX_PAID:
			publishHopperEvent("{\"event\":\"cashbox paid\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_JAMMED:
			publishHopperEvent("{\"event\":\"jammed\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			publishHopperEvent("{\"event\":\"fraud attempt\"}");
			break;
		case SSP_POLL_COIN_CREDIT:
			publishHopperEvent("{\"event\":\"coin credit\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_EMPTY:
			publishHopperEvent("{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishHopperEvent("{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishHopperEvent("{\"event\":\"smart emptying\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_SMART_EMPTIED:
			publishHopperEvent("{\"event\":\"smart emptied\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			publishHopperEvent("{\"event\":\"credit\",\"channel\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishHopperEvent(
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishHopperEvent(
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_DISABLED:
			// The unit has been disabled
			publishHopperEvent("{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishHopperEvent("{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			publishHopperEvent("{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

/**
 *  Callback function used for publishing events reported by the Validator hardware.
 */
void validatorEventHandler(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {

	for (int i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishValidatorEvent("{\"event\":\"unit reset\"}");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the \"read\" event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				// The note which was in escrow has been accepted
				unsigned long amount =
						device->sspSetupReq.ChannelData[poll->events[i].data1 - 1].value
								* 100;
				publishValidatorEvent("{\"event\":\"read\",\"amount\":%ld,\"channel\":%ld}",
						amount, poll->events[i].data1);
			} else {
				publishValidatorEvent("{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_EMPTY:
			publishValidatorEvent("{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishValidatorEvent("{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishValidatorEvent("{\"event\":\"smart emptying\"}");
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
		{
			unsigned long amount =
					device->sspSetupReq.ChannelData[poll->events[i].data1 - 1].value
							* 100;
			publishValidatorEvent("{\"event\":\"credit\",\"amount\":%ld,\"channel\":%ld}",
					amount, poll->events[i].data1);
		}
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishValidatorEvent(
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishValidatorEvent(
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_REJECTING:
			publishValidatorEvent("{\"event\":\"rejecting\"}");
			break;
		case SSP_POLL_REJECTED:
			// The note was rejected
			publishValidatorEvent("{\"event\":\"rejected\"}");
			break;
		case SSP_POLL_STACKING:
			publishValidatorEvent("{\"event\":\"stacking\"}");
			break;
		case SSP_POLL_STORED:
			// The note has been stored in the payout unit
			publishValidatorEvent("{\"event\":\"stored\"}");
			break;
		case SSP_POLL_STACKED:
			// The note has been stacked in the cashbox
			publishValidatorEvent("{\"event\":\"stacked\"}");
			break;
		case SSP_POLL_SAFE_JAM:
			publishValidatorEvent("{\"event\":\"safe jam\"}");
			break;
		case SSP_POLL_UNSAFE_JAM:
			publishValidatorEvent("{\"event\":\"unsafe jam\"}");
			break;
		case SSP_POLL_DISABLED:
			// The validator has been disabled
			publishValidatorEvent("{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			// The validator has detected a fraud attempt
			publishValidatorEvent(
					"{\"event\":\"fraud attempt\",\"dispensed\":%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_STACKER_FULL:
			// The cashbox is full
			publishValidatorEvent("{\"event\":\"stacker full\"}");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			// The cashbox has been removed
			publishValidatorEvent("{\"event\":\"cashbox removed\"}");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			// The cashbox has been replaced
			publishValidatorEvent("{\"event\":\"cashbox replaced\"}");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			// A note was in the notepath at startup and has been cleared from the front of the validator
			publishValidatorEvent("{\"event\":\"cleared from front\"}");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			// A note was in the notepath at startup and has been cleared into the cashbox
			publishValidatorEvent("{\"event\":\"cleared into cashbox\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishValidatorEvent("{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			publishValidatorEvent("{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

/**
 * Initializes and configures redis, libevent and the hardware.
 */
void setup(struct m_metacash *metacash) {
	// initialize libEvent
	metacash->eventBase = event_base_new();

	// connect to redis
	redisPublishCtx = connectRedis(metacash); // establish connection for publishing
	redisSubscribeCtx = connectRedis(metacash); // establich connection for subscribing

	// setup redis
	if (redisPublishCtx && redisSubscribeCtx) {
		redisLibeventAttach(redisPublishCtx, metacash->eventBase);
		redisAsyncSetConnectCallback(redisPublishCtx, cbOnConnectPublishContext);
		redisAsyncSetDisconnectCallback(redisPublishCtx, cbOnDisconnectPublishContext);

		redisLibeventAttach(redisSubscribeCtx, metacash->eventBase);
		redisAsyncSetConnectCallback(redisSubscribeCtx, cbOnConnectSubscribeContext);
		redisAsyncSetDisconnectCallback(redisSubscribeCtx, cbOnDisconnectSubscribeContext);
	} else {
		printf("fatal: could not establish connection to redis.\n");
		exit(1);
	}

	// setup libevent triggered check if we should quit (every 500ms more or less)
	{
		struct timeval interval;
		interval.tv_sec = 0;
		interval.tv_usec = 500000;

		event_set(&metacash->evCheckQuit, 0, EV_PERSIST, cbOnCheckQuitEvent, metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evCheckQuit);
		evtimer_add(&metacash->evCheckQuit, &interval);
	}

	// try to initialize the hardware only if we successfully have opened the device
	if (metacash->deviceAvailable) {
		// prepare the device structures
		mcSspSetupCommand(&metacash->validator.sspC, metacash->validator.id);
		mcSspSetupCommand(&metacash->hopper.sspC, metacash->hopper.id);

		// initialize the devices
		printf("\n");
		mcSspInitializeDevice(&metacash->validator.sspC,
				metacash->validator.key, &metacash->validator);
		printf("\n");
		mcSspInitializeDevice(&metacash->hopper.sspC, metacash->hopper.key,
				&metacash->hopper);
		printf("\n");

		{
			// SMART Hopper configuration
			int i;
			for (i = 0; i < metacash->hopper.sspSetupReq.NumberOfChannels; i++) {
				ssp6_set_coinmech_inhibits(&metacash->hopper.sspC,
						metacash->hopper.sspSetupReq.ChannelData[i].value,
						metacash->hopper.sspSetupReq.ChannelData[i].cc, ENABLED);
			}
		}

		{
			// SMART Payout configuration

			// reject notes unfit for storage.
			// if this is not enabled, notes unfit for storage will be silently redirected
			// to the cashbox of the validator from which no payout can be done.
			if (mc_ssp_set_refill_mode(&metacash->validator.sspC)
					!= SSP_RESPONSE_OK) {
				printf("ERROR: setting refill mode failed\n");
			}

			// setup the routing of the banknotes in the validator (amounts are in cent)
			ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY,
					ROUTE_CASHBOX); // 5 euro
			ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY,
					ROUTE_CASHBOX); // 10 euro
			ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY,
					ROUTE_CASHBOX); // 20 euro
			ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY,
					ROUTE_STORAGE); // 50 euro
			ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY,
					ROUTE_STORAGE); // 100 euro
			ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY,
					ROUTE_STORAGE); // 200 euro
			ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY,
					ROUTE_STORAGE); // 500 euro

			metacash->validator.channelInhibits = 0x0; // disable all channels

			// set the inhibits in the hardware
			if (ssp6_set_inhibits(&metacash->validator.sspC, metacash->validator.channelInhibits, 0x0)
					!= SSP_RESPONSE_OK) {
				printf("ERROR: Inhibits Failed\n");
				return;
			}

			//enable the payout unit
			if (ssp6_enable_payout(&metacash->validator.sspC,
					metacash->validator.sspSetupReq.UnitType)
					!= SSP_RESPONSE_OK) {
				printf("ERROR: Enable Payout Failed\n");
				return;
			}
		}

		printf("setup finished successfully\n");
	}

	// setup libevent triggered polling of the hardware (every second more or less)
	{
		struct timeval interval;
		interval.tv_sec = 1;
		interval.tv_usec = 0;

		event_set(&metacash->evPoll, 0, EV_PERSIST, cbOnPollEvent, metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evPoll);
		evtimer_add(&metacash->evPoll, &interval);
	}
}

/**
 * Opens the given serial device.
 */
int mcSspOpenSerialDevice(struct m_metacash *metacash) {
	// open the serial device
	printf("opening serial device: %s\n", metacash->serialDevice);

	{
		struct stat buffer;
		int fildes = open(metacash->serialDevice, O_RDWR);
		if (fildes <= 0) {
			printf("ERROR: opening device %s failed: %s\n", metacash->serialDevice, strerror(errno));
			return 1;
		}

		fstat(fildes, &buffer); // TODO: error handling

		close(fildes);

		switch (buffer.st_mode & S_IFMT) {
		case S_IFCHR:
			break;
		default:
			printf("ERROR: %s is not a device\n", metacash->serialDevice);
			return 1;
		}
	}

	if (open_ssp_port(metacash->serialDevice) == 0) {
		printf("ERROR: could not open serial device %s\n",
				metacash->serialDevice);
		return 1;
	}
	return 0;
}

/**
 * Closes the given serial device.
 */
void mcSspCloseSerialDevice(struct m_metacash *metacash) {
	close_ssp_port();
}

/**
 * Issues a poll command to the hardware and dispatches the response to the event handler function of the device.
 */
void mcSspPollDevice(struct m_device *device, struct m_metacash *metacash) {
	SSP_POLL_DATA6 poll;

	hardwareWaitTime();

	// poll the unit
	SSP_RESPONSE_ENUM resp;
	if ((resp = ssp6_poll(&device->sspC, &poll)) != SSP_RESPONSE_OK) {
		if (resp == SSP_RESPONSE_TIMEOUT) {
			// If the poll timed out, then give up
			printf("SSP Poll Timeout\n");
			return;
		} else {
			if (resp == SSP_RESPONSE_KEY_NOT_SET) {
				// The unit has responded with key not set, so we should try to negotiate one
				if (ssp6_setup_encryption(&device->sspC, device->key)
						!= SSP_RESPONSE_OK) {
					printf("Encryption Failed\n");
				} else {
					printf("Encryption Setup\n");
				}
			} else {
				printf("SSP Poll Error: 0x%x\n", resp);
			}
		}
	} else {
		if (poll.event_count > 0) {
			printf("parsing poll response from \"%s\" now (%d events)\n",
					device->name, poll.event_count);
			device->eventHandlerFn(device, metacash, &poll);
		} else {
			//printf("polling \"%s\" returned no events\n", device->name);
		}
	}
}

/**
 * Initializes an ITL hardware device via SSP
 */
void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key,
		struct m_device *device) {
	SSP6_SETUP_REQUEST_DATA *sspSetupReq = &device->sspSetupReq;
	unsigned int i = 0;

	printf("initializing device (id=0x%02X, '%s')\n", sspC->SSPAddress, device->name);

	//check device is present
	if (ssp6_sync(sspC) != SSP_RESPONSE_OK) {
		printf("ERROR: No device found\n");
		return;
	}
	printf("device found\n");

	//try to setup encryption using the default key
	if (ssp6_setup_encryption(sspC, key) != SSP_RESPONSE_OK) {
		printf("ERROR: Encryption failed\n");
		return;
	}
	printf("encryption setup\n");

	// Make sure we are using ssp version 6
	if (ssp6_host_protocol(sspC, 0x06) != SSP_RESPONSE_OK) {
		printf("ERROR: Host Protocol Failed\n");
		return;
	}
	printf("host protocol verified\n");

	// Collect some information about the device
	if (ssp6_setup_request(sspC, sspSetupReq) != SSP_RESPONSE_OK) {
		printf("ERROR: Setup Request Failed\n");
		return;
	}

	printf("channels:\n");
	for (i = 0; i < sspSetupReq->NumberOfChannels; i++) {
		printf("channel %d: %d %s\n", i + 1, sspSetupReq->ChannelData[i].value,
				sspSetupReq->ChannelData[i].cc);
	}

	char version[100];
	mc_ssp_get_firmware_version(sspC, &version[0]);
	printf("full firmware version: %s\n", version);

	mc_ssp_get_dataset_version(sspC, &version[0]);
	printf("full dataset version : %s\n", version);

	//enable the device
	if (ssp6_enable(sspC) != SSP_RESPONSE_OK) {
		printf("ERROR: Enable Failed\n");
		return;
	}

	printf("device has been successfully initialized\n");
}

/**
 * Initializes the SSP_COMMAND structure.
 */
void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId) {
	sspC->SSPAddress = deviceId;
	sspC->Timeout = 1000;
	sspC->EncryptionStatus = NO_ENCRYPTION;
	sspC->RetryLevel = 3;
	sspC->BaudRate = 9600;
}

/**
 * Implements the "LAST REJECT NOTE" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC,
		unsigned char *reason) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_LAST_REJECT_NOTE;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	*reason = sspC->ResponseData[1];

	return resp;
}

/**
 * Implements the "DISPLAY ON" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_DISPLAY_ON;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "DISPLAY OFF" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_DISPLAY_OFF;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "SET REFILL MODE" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 9;
	sspC->CommandData[0] = SSP_CMD_SET_REFILL_MODE;

	sspC->CommandData[1] = 0x05;
	sspC->CommandData[2] = 0x81;
	sspC->CommandData[3] = 0x10;
	sspC->CommandData[4] = 0x11;
	sspC->CommandData[5] = 0x01;
	sspC->CommandData[6] = 0x01;
	sspC->CommandData[7] = 0x52;
	sspC->CommandData[8] = 0xF5;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "EMPTY" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_EMPTY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "SMART EMPTY" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_smart_empty(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_SMART_EMPTY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "CONFIGURE BEZEL" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char non_volatile) {
	sspC->CommandDataLength = 5;
	sspC->CommandData[0] = SSP_CMD_CONFIGURE_BEZEL;
	sspC->CommandData[1] = r;
	sspC->CommandData[2] = g;
	sspC->CommandData[3] = b;
	sspC->CommandData[4] = non_volatile;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "SET DENOMINATION LEVEL" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, const char *cc) {
	sspC->CommandDataLength = 10;
	sspC->CommandData[0] = SSP_CMD_SET_DENOMINATION_LEVEL;

	int j = 0;
	int i;

	for (i = 0; i < 2; i++) {
		sspC->CommandData[++j] = level >> (i * 8);
	}

	for (i = 0; i < 4; i++) {
		sspC->CommandData[++j] = amount >> (i * 8);
	}

	for (i = 0; i < 3; i++) {
		sspC->CommandData[++j] = cc[i];
	}

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// not data to parse

	return resp;
}

/**
 * Implements the "GET ALL LEVELS" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_all_levels(SSP_COMMAND *sspC, char **json) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_ALL_LEVELS;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	/* The first data byte oin the response is the number of counters returned. Each counter consists of 9 bytes of
	 * data made up as: 2 bytes giving the denomination level, 4 bytes giving the value and 3 bytes of ascii country
	 * code.
	 */

	int i = 0;

	i++; // move onto numCounters
	int numCounters = sspC->ResponseData[i];

	/* Create StringBuffer 'object' (struct) */
	SB *sb = getStringBuffer();

	int j; // current counter
	for (j = 0; j < numCounters; ++j) {
		int k;

		int value = 0;
		int level = 0;
		char cc[4] = {0};

		for (k = 0; k < 2; ++k) {
			i++; //move through the 2 bytes of data
			level +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 4; ++k) {
			i++; //move through the 4 bytes of data
			value +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 3; ++k) {
			i++; //move through the 3 bytes of country code
			cc[k] =
					sspC->ResponseData[i];
		}

		char *response = NULL;
		asprintf(&response,
				"{\"value\":%d,\"level\":%d,\"cc\":\"%s\"}", value, level, cc);

		if(j > 0) {
			char *sep = ",";
			sb->append( sb, sep); // json array seperator
		}
		sb->append( sb, response);

		free(response);
	}

	/* Call toString() function to get catenated list */
	char *result = sb->toString( sb );

	asprintf(json, "%s", result);

	/* Dispose of StringBuffer's memory */
	sb->dispose( &sb ); /* Note: Need to pass ADDRESS of struct pointer to dispose() */

	return resp;
}

/**
 * Implements the "FLOAT" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value,
		const char *cc, const char option) {
	int i;

	sspC->CommandDataLength = 11;
	sspC->CommandData[0] = SSP_CMD_FLOAT;

	int j = 0;

	// minimum requested value to float
	for (i = 0; i < 2; i++) {
		sspC->CommandData[++j] = 100 >> (i * 8); // min 1 euro
	}

	// amount to keep for payout
	for (i = 0; i < 4; i++) {
		sspC->CommandData[++j] = value >> (i * 8) ;
	}

	for (i = 0; i < 3; i++) {
		sspC->CommandData[++j] = cc[i];
	}

	sspC->CommandData[++j] = option;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * Implements the "GET FIRMWARE VERSION" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_firmware_version(SSP_COMMAND *sspC, char *firmwareVersion) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_FIRMWARE_VERSION;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		for(int i = 0; i < 16; i++) {
			*(firmwareVersion + i) = sspC->ResponseData[1 + i];
		}
		*(firmwareVersion + 16) = 0;
	}

	return resp;
}

/**
 * Implements the "GET DATASET VERSION" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_dataset_version(SSP_COMMAND *sspC, char *datasetVersion) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_DATASET_VERSION;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		for(int i = 0; i < 8; i++) {
			*(datasetVersion + i) = sspC->ResponseData[1 + i];
		}
		*(datasetVersion + 8) = 0;
	}

	return resp;
}

/**
 * Implements the "CHANNEL SECURITY DATA" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_channel_security_data(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_CHANNEL_SECURITY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		int numChannels = sspC->ResponseData[1];

		printf("security status: numChannels=%d\n", numChannels);
		printf("0 = unused, 1 = low, 2 = std, 3 = high, 4 = inhibited\n");
		for(int i = 0; i < numChannels; i++) {
			printf("security status: channel %d -> %d\n", 1 + i, sspC->ResponseData[2 + i]);
		}
	}

	return resp;
}
