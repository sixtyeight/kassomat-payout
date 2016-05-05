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

struct m_metacash;

struct m_device {
	int id;
	char *name;
	unsigned long long key;
	unsigned char channelInhibits;

	SSP_COMMAND sspC;
	SSP6_SETUP_REQUEST_DATA setup_req;
	void (*parsePoll)(struct m_device *device, struct m_metacash *metacash,
			SSP_POLL_DATA6 *poll);
};

struct m_metacash {
	int quit;
	int deviceAvailable;
	char *serialDevice;

	int redisPort;
	char *redisHost;
	redisAsyncContext *db; // redis context used for persistence
	redisAsyncContext *pubSub; // redis context used for messaging (publish / subscribe)

	struct event_base *eventBase; // libevent
	struct event evPoll; // event for periodically polling the cash hardware
	struct event evCheckQuit; // event for periodically checking to quit

	struct m_device hopper; // smart hopper device
	struct m_device validator; // nv200 + smart payout devices
};

// mcSsp* : ssp helper functions
int mcSspOpenSerialDevice(struct m_metacash *metacash);
void mcSspCloseSerialDevice(struct m_metacash *metacash);
void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId);
void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key, struct m_device *device);
void mcSspPollDevice(struct m_device *device, struct m_metacash *metacash);

// mc_ssp_* : ssp functions (each of these relate directly to a command specified in the ssp protocol)
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_smart_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r, unsigned char g, unsigned char b, unsigned char non_volatile);
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC, unsigned char *reason);
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_all_levels(SSP_COMMAND *sspC, char **json);
SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, char *cc);
SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value, const char *cc, const char option);
SSP_RESPONSE_ENUM mc_ssp_channel_security_data(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_firmware_version(SSP_COMMAND *sspC, char *firmwareVersion);
SSP_RESPONSE_ENUM mc_ssp_get_dataset_version(SSP_COMMAND *sspC, char *datasetVersion);

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash);
void mcSetup(struct m_metacash *metacash);

// cash hardware result parsing
void mcHandleEventsHopper(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void mcHandleEventsValidator(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

static const char *CURRENCY = "EUR";
static const char ROUTE_CASHBOX = 0x01;
static const char ROUTE_STORAGE = 0x00;
static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

#define SSP_CMD_GET_FIRMWARE_VERSION 0x20
#define SSP_CMD_GET_DATASET_VERSION 0x21
#define SSP_CMD_GET_ALL_LEVELS 0x22
#define SSP_CMD_SET_DENOMINATION_LEVEL 0x34
#define SSP_CMD_LAST_REJECT_NOTE 0x17
#define SSP_CMD_CONFIGURE_BEZEL 0x54
#define SSP_CMD_SMART_EMPTY 0x52
#define SSP_CMD_SET_REFILL_MODE 0x30
#define SSP_CMD_DISPLAY_OFF 0x4
#define SSP_CMD_DISPLAY_ON 0x3

int receivedSignal = 0;

void interrupt(int signal) {
	receivedSignal = signal;
}

void hardwareWaitTime() {
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 300000000;
	nanosleep(&ts, NULL);
}

redisAsyncContext* mcConnectRedis(struct m_metacash *metacash) {
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

void cbPollEvent(int fd, short event, void *privdata) {
	struct m_metacash *metacash = privdata;
	if (metacash->deviceAvailable == 0) {
		// return immediately if we have no actual hardware to poll
		return;
	}

	mcSspPollDevice(&metacash->hopper, metacash);
	mcSspPollDevice(&metacash->validator, metacash);
}

void cbCheckQuit(int fd, short event, void *privdata) {
	if (receivedSignal != 0) {
		syslog(LOG_NOTICE, "received signal. going to exit event loop.");

		struct m_metacash *metacash = privdata;
		event_base_loopexit(metacash->eventBase, NULL);
		receivedSignal = 0;
	}
}

void cbOnMetacashMessage(redisAsyncContext *c, void *r, void *privdata) {
	// empty for now
}

/* isCommand: Test if the message contains the "cmd":"(command)" property */
int isCommand(char *message, const char *command) {
	char *commandPattern;

	asprintf(&commandPattern, "\"cmd\":\"%s\"", command);
	char *found = strstr(message, commandPattern);
	free(commandPattern);

	return found != 0;
}

int publishHopperEvent(redisAsyncContext *c, char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(c, NULL, NULL, "PUBLISH %s %s", "hopper-event", reply);

	free(reply);

	return 0;
}

int publishValidatorEvent(redisAsyncContext *c, char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(c, NULL, NULL, "PUBLISH %s %s", "validator-event", reply);

	free(reply);

	return 0;
}

int replyWith(redisAsyncContext *c, char *topic, char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(c, NULL, NULL, "PUBLISH %s %s", topic, reply);

	free(reply);

	return 0;
}

int replyOk(redisAsyncContext *c, char *topic, char *responseMsgId, char *msgId) {
	return replyWith(c, topic,
			"{\"msgId\":\"%s\",\"correlId\":\"%s\",\"result\":\"ok\"}",
			responseMsgId, msgId);
}

int replyFailed(redisAsyncContext *c, char *topic, char *responseMsgId, char *msgId) {
	return replyWith(c, topic,
			"{\"msgId\":\"%s\",\"correlId\":\"%s\",\"result\":\"failed\"}",
			responseMsgId, msgId);
}

int replyAccepted(redisAsyncContext *c, char *topic, char *responseMsgId, char *msgId) {
	return replyWith(c, topic,
			"{\"msgId\":\"%s\",\"correlId\":\"%s\",\"accepted\":\"true\"}",
			responseMsgId, msgId);
}

void cbOnRequestMessage(redisAsyncContext *c, void *r, void *privdata) {
	if (r == NULL)
		return;

	hardwareWaitTime();

	struct m_metacash *m = c->data;
	redisReply *reply = r;

	// example from http://stackoverflow.com/questions/16213676/hiredis-waiting-for-message
	if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
		if (strcmp(reply->element[0]->str, "subscribe") != 0) {
			char *topic = reply->element[1]->str;
			struct m_device *device = NULL;

			char *response_topic = NULL;
			if (strcmp(topic, "validator-request") == 0) {
				device = &m->validator;
				response_topic = "validator-response";
			} else if (strcmp(topic, "hopper-request") == 0) {
				device = &m->hopper;
				response_topic = "hopper-response";
			} else {
				return;
			}

			char *message = reply->element[2]->str;
			redisAsyncContext *db = m->db;

			const char *msgIdToken = "\"msgId\":\"";
			char *msgIdStart = strstr(message, msgIdToken);
			if (msgIdStart == NULL) {
				replyWith(db, response_topic, "{\"error\":\"msgId missing\"}");
				return;
			}
			msgIdStart = msgIdStart + strlen(msgIdToken);
			char *msgIdEnd = strstr(msgIdStart, "\"");
			if (msgIdEnd == NULL) {
				replyWith(db, response_topic, "{\"error\":\"msgId not a string\"}");
				return;
			}

			char *msgId = strndup(msgIdStart, (msgIdEnd - msgIdStart));

			// generate a new msgId for the response
			uuid_t uuid;
			uuid_generate_time_safe(uuid);
			char responseMsgId[37] = { 0 }; // ex. "1b4e28ba-2fa1-11d2-883f-0016d3cca427" + "\0"
			uuid_unparse_lower(uuid, responseMsgId);

			printf("responding with correlId=\"%s\" and msgId=\"%s\"\n", msgId,
					responseMsgId);

			if(isCommand(message, "quit")) {
				replyOk(db, response_topic, responseMsgId, msgId);
				receivedSignal = 1;
			} else if (isCommand(message, "empty")) {
				mc_ssp_empty(&device->sspC);

				replyAccepted(db, response_topic, responseMsgId, msgId);
			} else if (isCommand(message, "smart-empty")) {
				mc_ssp_smart_empty(&device->sspC);

				replyAccepted(db, response_topic, responseMsgId, msgId);
			} else if (isCommand(message, "enable")) {
				ssp6_enable(&device->sspC);

				replyAccepted(db, response_topic, responseMsgId, msgId);
			} else if (isCommand(message, "disable")) {
				ssp6_disable(&device->sspC);

				replyAccepted(db, response_topic, responseMsgId, msgId);
			} else if(isCommand(message, "enable-channels")) {
				const char *channelsToken = "\"channels\":\"";

				char *channelsStart = strstr(message, channelsToken);
				if (channelsStart == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels missing\"}");
					return;
				}
				channelsStart= channelsStart + strlen(channelsToken);
				char *channelsEnd = strstr(channelsStart, "\"");
				if (channelsEnd == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels not a string\"}");
					return;
				}

				// this will be updated and written back to the device state
				// if the update succeeds
				unsigned char currentChannelInhibits = device->channelInhibits;

				char *channels = strndup(channelsStart, (channelsEnd - channelsStart));
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

				SSP_RESPONSE_ENUM r = ssp6_set_inhibits(&device->sspC, currentChannelInhibits, highChannels);

				if(r == SSP_RESPONSE_OK) {
					// okay, update the channelInhibits in the device structure with the new state
					device->channelInhibits = currentChannelInhibits;

					if(0) {
						printf("enable-channels: current inhibits now: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d\n",
								(currentChannelInhibits >> 0) & 1,
								(currentChannelInhibits >> 1) & 1,
								(currentChannelInhibits >> 2) & 1,
								(currentChannelInhibits >> 3) & 1,
								(currentChannelInhibits >> 4) & 1,
								(currentChannelInhibits >> 5) & 1,
								(currentChannelInhibits >> 6) & 1,
								(currentChannelInhibits >> 7) & 1);
					}

					replyOk(db, response_topic, responseMsgId, msgId);
				} else {
					replyFailed(db, response_topic, responseMsgId, msgId);
				}

				free(channels);
			} else if(isCommand(message, "disable-channels")) {
				const char *channelsToken = "\"channels\":\"";

				char *channelsStart = strstr(message, channelsToken);
				if (channelsStart == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels missing\"}");
					return;
				}
				channelsStart= channelsStart + strlen(channelsToken);
				char *channelsEnd = strstr(channelsStart, "\"");
				if (channelsEnd == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels not a string\"}");
					return;
				}

				// this will be updated and written back to the device state
				// if the update succeeds
				unsigned char currentChannelInhibits = device->channelInhibits;

				char *channels = strndup(channelsStart, (channelsEnd - channelsStart));
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

				SSP_RESPONSE_ENUM r = ssp6_set_inhibits(&device->sspC, currentChannelInhibits, highChannels);

				if(r == SSP_RESPONSE_OK) {
					// okay, update the channelInhibits in the device structure with the new state
					device->channelInhibits = currentChannelInhibits;

					if(0) {
						printf("disable-channels: current inhibits now: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d\n",
								(currentChannelInhibits >> 0) & 1,
								(currentChannelInhibits >> 1) & 1,
								(currentChannelInhibits >> 2) & 1,
								(currentChannelInhibits >> 3) & 1,
								(currentChannelInhibits >> 4) & 1,
								(currentChannelInhibits >> 5) & 1,
								(currentChannelInhibits >> 6) & 1,
								(currentChannelInhibits >> 7) & 1);
					}

					replyOk(db, response_topic, responseMsgId, msgId);
				} else {
					replyFailed(db, response_topic, responseMsgId, msgId);
				}

				free(channels);
			} else if(isCommand(message, "inhibit-channels")) {
				const char *channelsToken = "\"channels\":\"";

				char *channelsStart = strstr(message, channelsToken);
				if (channelsStart == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels missing\"}");
					return;
				}
				channelsStart= channelsStart + strlen(channelsToken);
				char *channelsEnd = strstr(channelsStart, "\"");
				if (channelsEnd == NULL) {
					replyWith(db, response_topic, "{\"error\":\"channels not a string\"}");
					return;
				}

				char *channels = strndup(channelsStart, (channelsEnd - channelsStart));
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

				SSP_RESPONSE_ENUM r = ssp6_set_inhibits(&device->sspC, lowChannels, highChannels);

				if(r == SSP_RESPONSE_OK) {
					replyOk(db, response_topic, responseMsgId, msgId);
				} else {
					replyFailed(db, response_topic, responseMsgId, msgId);
				}

				free(channels);
			} else if (isCommand(message, "test-float")
					|| isCommand(message, "do-float")) {
				// basically a copy of do/test-payout ...

				int payoutOption = 0;

				if (isCommand(message, "do-float")) {
					payoutOption = SSP6_OPTION_BYTE_DO;
				} else {
					payoutOption = SSP6_OPTION_BYTE_TEST;
				}

				char *amountToken = "\"amount\":";
				char *amountStart = strstr(message, amountToken);
				if (amountStart != NULL) {
					amountStart = amountStart + strlen(amountToken);
					int amount = atoi(amountStart);

					if (mc_ssp_float(&device->sspC, amount, CURRENCY,
							payoutOption) != SSP_RESPONSE_OK) {
						// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
						redisAsyncContext *db = m->db;
						char *error = NULL;
						switch (device->sspC.ResponseData[1]) {
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
						replyWith(db, response_topic, "{\"correlId\":\"%s\",\"error\":\"%s\"}", msgId, error);
					} else {
						replyOk(db, response_topic, responseMsgId, msgId);
					}
				}
			} else if (isCommand(message, "test-payout")
					|| isCommand(message, "do-payout")) {
				int payoutOption = 0;

				if (isCommand(message, "do-payout")) {
					payoutOption = SSP6_OPTION_BYTE_DO;
				} else {
					payoutOption = SSP6_OPTION_BYTE_TEST;
				}

				char *amountToken = "\"amount\":";
				char *amountStart = strstr(message, amountToken);
				if (amountStart != NULL) {
					amountStart = amountStart + strlen(amountToken);
					int amount = atoi(amountStart);

					if (ssp6_payout(&device->sspC, amount, CURRENCY,
							payoutOption) != SSP_RESPONSE_OK) {
						// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
						redisAsyncContext *db = m->db;
						char *error = NULL;
						switch (device->sspC.ResponseData[1]) {
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

						replyWith(db, response_topic, "{\"correlId\":\"%s\",\"error\":\"%s\"}", msgId, error);
					} else {
						replyOk(db, response_topic, responseMsgId, msgId);
					}
				}
			} else if (isCommand(message, "get-firmware-version")) {
				char firmwareVersion[100] = { 0 };
				mc_ssp_get_firmware_version(&device->sspC, &firmwareVersion[0]);
				replyWith(db, response_topic,"{\"correlId\":\"%s\",\"version\":\"%s\"]}", msgId, firmwareVersion);
			} else if (isCommand(message, "get-dataset-version")) {
				char datasetVersion[100] = { 0 };
				mc_ssp_get_dataset_version(&device->sspC, &datasetVersion[0]);
				replyWith(db, response_topic,"{\"correlId\":\"%s\",\"version\":\"%s\"]}", msgId, datasetVersion);
			} else if (isCommand(message, "channel-security")) {
				mc_ssp_channel_security_data(&device->sspC);
			} else if (isCommand(message, "get-all-levels")) {
				char *json = NULL;
				mc_ssp_get_all_levels(&device->sspC, &json);
				replyWith(db, response_topic,"{\"correlId\":\"%s\",\"levels\":[%s]}", msgId, json);
				free(json);
			} else if (isCommand(message, "set-denomination-level")) {
				char *levelToken = "\"level\":";
				char *levelStart = strstr(message, levelToken);
				int level;
				if (levelStart != NULL) {
					levelStart = levelStart + strlen(levelToken);
					level = atoi(levelStart);
				}

				char *amountToken = "\"amount\":";
				char *amountStart = strstr(message, amountToken);
				int amount;
				if (amountStart != NULL) {
					amountStart = amountStart + strlen(amountToken);
					amount = atoi(amountStart);
				}

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

					mc_ssp_set_denomination_level(&device->sspC, amount, 0, "EUR"); // ignore the result for now
				}

				if (mc_ssp_set_denomination_level(&device->sspC, amount, level, "EUR") == SSP_RESPONSE_OK) {
					replyOk(db, response_topic, responseMsgId, msgId);
				} else {
					replyFailed(db, response_topic, responseMsgId, msgId);
				}
			} else if (isCommand(message, "last-reject-note")) {
				unsigned char reasonCode;
				char *reason = NULL;

				if (mc_ssp_last_reject_note(&device->sspC, &reasonCode)
						== SSP_RESPONSE_OK) {
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
						break;
					}
					if (reason != NULL) {
						replyWith(db, response_topic, "{\"correlId\":\"%s\",\"reason\":\"%s\",\"code\":%ld}", msgId, reason, reasonCode);
					} else {
						replyWith(db, response_topic, "{\"correlId\":\"%s\",\"reason\":\"undefined\",\"code\":%ld}", msgId, reasonCode);
					}
				} else {
					replyWith(db, response_topic, "{\"timeout\":\"last reject note\"}");
				}
			} else {
				replyWith(db, response_topic, "{\"error\":\"cmd missing\"}", message);
			}

			free(msgId);
		}
	}
}

void cbConnectDb(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "Connected to database...\n");
}

void cbDisconnectDb(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "Disconnected from database\n");
}

void cbConnectPubSub(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "PubSub - Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "PubSub - Connected to database...\n");

	redisAsyncContext *cNotConst = (redisAsyncContext*) c; // get rids of discarding qualifier \"const\" warning

	redisAsyncCommand(cNotConst, cbOnMetacashMessage, NULL,
			"SUBSCRIBE metacash");
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL,
			"SUBSCRIBE validator-request");
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL,
			"SUBSCRIBE hopper-request");
}

void cbDisconnectPubSub(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "PubSub - Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "PubSub - Disconnected from database\n");
}

int main(int argc, char *argv[]) {
	// setup logging via syslog
	setlogmask(LOG_UPTO(LOG_NOTICE));
	openlog("metacashd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	syslog(LOG_NOTICE, "Program started by User %d", getuid());

	// register interrupt handler for signals
	signal(SIGTERM, interrupt);
	signal(SIGINT, interrupt);

	struct m_metacash metacash;
	metacash.deviceAvailable = 0;
	metacash.quit = 0;

	metacash.serialDevice = "/dev/ttyACM0";	// default, override with -d argument
	metacash.redisHost = "127.0.0.1";	// default, override with -h argument
	metacash.redisPort = 6379;			// default, override with -p argument

	metacash.hopper.id = 0x10; // 0X10 -> Smart Hopper ("Münzer")
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.parsePoll = mcHandleEventsHopper;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.parsePoll = mcHandleEventsValidator;

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
	mcSetup(&metacash);

	syslog(LOG_NOTICE, "metacash open for business :D");

	event_base_dispatch(metacash.eventBase); // blocking until exited via api-call

	syslog(LOG_NOTICE, "exiting");

	if (metacash.deviceAvailable) {
		mcSspCloseSerialDevice(&metacash);
	}

	// cleanup stuff before exiting.

	// redis
	redisAsyncFree(metacash.db);
	redisAsyncFree(metacash.pubSub);

	// libevent
	event_base_free(metacash.eventBase);

	// syslog
	closelog();

	return 0;
}

int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash) {
	opterr = 0;

	char c;
	while ((c = getopt(argc, argv, "h:p:d:")) != -1)
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
			if (optopt == 'h' || optopt == 'p' || optopt == 'd')
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option '-%c'.\n", optopt);
			else
				fprintf(stderr, "Unknown option character 'x%x'.\n", optopt);
			return 1;
		default:
			return 1;
		}

	return 0;
}

// business stuff
void mcHandleEventsHopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	redisAsyncContext *db = metacash->db;

	int i;
	for (i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishHopperEvent(db, "{\"event\":\"unit reset\"}");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the \"read\" event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				publishHopperEvent(db, "{\"event\":\"read\",\"channel\":%ld}", poll->events[i].data1);
			} else {
				publishHopperEvent(db, "{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_DISPENSING:
			publishHopperEvent(db, "{\"event\":\"dispensing\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_DISPENSED:
			publishHopperEvent(db, "{\"event\":\"dispensed\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_FLOATING:
			publishHopperEvent(db, "{\"event\":\"floating\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_FLOATED:
			publishHopperEvent(db, "{\"event\":\"floated\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CASHBOX_PAID:
			publishHopperEvent(db, "{\"event\":\"cashbox paid\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_JAMMED:
			publishHopperEvent(db, "{\"event\":\"jammed\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			publishHopperEvent(db, "{\"event\":\"fraud attempt\"}");
			break;
		case SSP_POLL_COIN_CREDIT:
			publishHopperEvent(db, "{\"event\":\"coin credit\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_EMPTY:
			publishHopperEvent(db, "{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishHopperEvent(db, "{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishHopperEvent(db, "{\"event\":\"smart emptying\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_SMART_EMPTIED:
			publishHopperEvent(db, "{\"event\":\"smart emptied\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			publishHopperEvent(db, "{\"event\":\"credit\",\"channel\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishHopperEvent(db,
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishHopperEvent(db,
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_DISABLED:
			// The unit has been disabled
			publishHopperEvent(db, "{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishHopperEvent(db, "{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishHopperEvent(db, "{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			publishHopperEvent(db, "{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

void mcHandleEventsValidator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	redisAsyncContext *db = metacash->db;

	for (int i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishValidatorEvent(db, "{\"event\":\"unit reset\"}");
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
						device->setup_req.ChannelData[poll->events[i].data1 - 1].value
								* 100;
				publishValidatorEvent(db, "{\"event\":\"read\",\"amount\":%ld,\"channel\":%ld}",
						amount, poll->events[i].data1);
			} else {
				publishValidatorEvent(db, "{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_EMPTY:
			publishValidatorEvent(db, "{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishValidatorEvent(db, "{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishValidatorEvent(db, "{\"event\":\"smart emptying\"}");
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
		{
			unsigned long amount =
					device->setup_req.ChannelData[poll->events[i].data1 - 1].value
							* 100;
			publishValidatorEvent(db, "{\"event\":\"credit\",\"amount\":%ld,\"channel\":%ld}",
					amount, poll->events[i].data1);
		}
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishValidatorEvent(db,
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishValidatorEvent(db,
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_REJECTING:
			publishValidatorEvent(db, "{\"event\":\"rejecting\"}");
			break;
		case SSP_POLL_REJECTED:
			// The note was rejected
			publishValidatorEvent(db, "{\"event\":\"rejected\"}");
			break;
		case SSP_POLL_STACKING:
			publishValidatorEvent(db, "{\"event\":\"stacking\"}");
			break;
		case SSP_POLL_STORED:
			// The note has been stored in the payout unit
			publishValidatorEvent(db, "{\"event\":\"stored\"}");
			break;
		case SSP_POLL_STACKED:
			// The note has been stacked in the cashbox
			publishValidatorEvent(db, "{\"event\":\"stacked\"}");
			break;
		case SSP_POLL_SAFE_JAM:
			publishValidatorEvent(db, "{\"event\":\"safe jam\"}");
			break;
		case SSP_POLL_UNSAFE_JAM:
			publishValidatorEvent(db, "{\"event\":\"unsafe jam\"}");
			break;
		case SSP_POLL_DISABLED:
			// The validator has been disabled
			publishValidatorEvent(db, "{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			// The validator has detected a fraud attempt
			publishValidatorEvent(db,
					"{\"event\":\"fraud attempt\",\"dispensed\":%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_STACKER_FULL:
			// The cashbox is full
			publishValidatorEvent(db, "{\"event\":\"stacker full\"}");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			// The cashbox has been removed
			publishValidatorEvent(db, "{\"event\":\"cashbox removed\"}");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			// The cashbox has been replaced
			publishValidatorEvent(db, "{\"event\":\"cashbox replaced\"}");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			// A note was in the notepath at startup and has been cleared from the front of the validator
			publishValidatorEvent(db, "{\"event\":\"cleared from front\"}");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			// A note was in the notepath at startup and has been cleared into the cashbox
			publishValidatorEvent(db, "{\"event\":\"cleared into cashbox\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishValidatorEvent(db, "{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishValidatorEvent(db, "{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			publishValidatorEvent(db, "{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

void mcSetup(struct m_metacash *metacash) {
	// initialize libEvent
	metacash->eventBase = event_base_new();

	// connect to redis
	metacash->db = mcConnectRedis(metacash); // establish connection for persistence
	metacash->pubSub = mcConnectRedis(metacash); // establich connection for pub/sub

	// setup redis
	if (metacash->db && metacash->pubSub) {
		redisLibeventAttach(metacash->db, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->db, cbConnectDb);
		redisAsyncSetDisconnectCallback(metacash->db, cbDisconnectDb);

		redisLibeventAttach(metacash->pubSub, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->pubSub, cbConnectPubSub);
		redisAsyncSetDisconnectCallback(metacash->pubSub, cbDisconnectPubSub);
	}

	// setup libevent triggered check if we should quit (every 500ms more or less)
	{
		struct timeval interval;
		interval.tv_sec = 0;
		interval.tv_usec = 500000;

		event_set(&metacash->evCheckQuit, 0, EV_PERSIST, cbCheckQuit, metacash); // provide metacash in privdata
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
			for (i = 0; i < metacash->hopper.setup_req.NumberOfChannels; i++) {
				ssp6_set_coinmech_inhibits(&metacash->hopper.sspC,
						metacash->hopper.setup_req.ChannelData[i].value,
						metacash->hopper.setup_req.ChannelData[i].cc, ENABLED);
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
					ROUTE_STORAGE); // 20 euro
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
					metacash->validator.setup_req.UnitType)
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

		event_set(&metacash->evPoll, 0, EV_PERSIST, cbPollEvent, metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evPoll);
		evtimer_add(&metacash->evPoll, &interval);
	}
}

int mcSspOpenSerialDevice(struct m_metacash *metacash) {
	// open the serial device
	printf("opening serial device: %s\n", metacash->serialDevice);

	{
		struct stat buffer;
		int fildes = open(metacash->serialDevice, O_RDWR);
		if (fildes == 0) {
			printf("ERROR: device %s not found\n", metacash->serialDevice);

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

void mcSspCloseSerialDevice(struct m_metacash *metacash) {
	close_ssp_port();
}

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
			device->parsePoll(device, metacash, &poll);
		} else {
			//printf("polling \"%s\" returned no events\n", device->name);
		}
	}
}

void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key,
		struct m_device *device) {
	SSP6_SETUP_REQUEST_DATA *setup_req = &device->setup_req;
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
	if (ssp6_setup_request(sspC, setup_req) != SSP_RESPONSE_OK) {
		printf("ERROR: Setup Request Failed\n");
		return;
	}

	//printf("firmware: %s\n", setup_req->FirmwareVersion);
	printf("channels:\n");
	for (i = 0; i < setup_req->NumberOfChannels; i++) {
		printf("channel %d: %d %s\n", i + 1, setup_req->ChannelData[i].value,
				setup_req->ChannelData[i].cc);
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

void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId) {
	sspC->SSPAddress = deviceId;
	sspC->Timeout = 1000;
	sspC->EncryptionStatus = NO_ENCRYPTION;
	sspC->RetryLevel = 3;
	sspC->BaudRate = 9600;
}

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

SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, char *cc) {
	sspC->CommandDataLength = 10;
	sspC->CommandData[0] = SSP_CMD_SET_DENOMINATION_LEVEL;

	int j = 0;
	int i;

	for (i = 0; i < 2; i++)
		sspC->CommandData[++j] = level >> (i * 8);

	for (i = 0; i < 4; i++)
		sspC->CommandData[++j] = amount >> (i * 8);

	for (i = 0; i < 3; i++)
		sspC->CommandData[++j] = cc[i];

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	return resp;
}

// get all the current levels in the device
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

SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value,
		const char *cc, const char option) {
	int i;

	sspC->CommandDataLength = 11;
	sspC->CommandData[0] = SSP_CMD_FLOAT;

	int j = 0;

	// minimum requested value to float
	for (i = 0; i < 2; i++)
		sspC->CommandData[++j] = 100 >> (i * 8); // min 1 euro

	// amount to keep for payout
	for (i = 0; i < 4; i++)
		sspC->CommandData[++j] = value >> (i * 8) ;

	for (i = 0; i < 3; i++)
		sspC->CommandData[++j] = cc[i];

	sspC->CommandData[++j] = option;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	return resp;
}

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

		return resp;
	} else {
		return resp;
	}
}
