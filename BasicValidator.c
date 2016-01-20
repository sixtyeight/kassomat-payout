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

#include "port_linux.h"
#include "ssp_defines.h"
#include "ssp_helpers.h"
#include "SSPComs.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>

#include <syslog.h>

struct m_credit {
	unsigned long amount;
};

struct m_metacash;

struct m_device {
	int id;
	char *name;
	unsigned long long key;

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
	redisAsyncContext *db;		// redis context used for persistence
	redisAsyncContext *pubSub;	// redis context used for messaging (publish / subscribe)

	struct event_base *eventBase;	// libevent
	struct event evPoll; 			// event for periodically polling the cash hardware
	struct event evCheckQuit;		// event for periodically checking to quit

	struct m_credit credit;

	struct m_device hopper;
	struct m_device validator;
};

// mc_ssp_* : metacash ssp functions (cash hardware, low level)
int mc_ssp_open_serial_device(struct m_metacash *metacash);
void mc_ssp_close_serial_device(struct m_metacash *metacash);
void mc_ssp_setup_command(SSP_COMMAND *sspC, int deviceId);
void mc_ssp_initialize_device(SSP_COMMAND *sspC, unsigned long long key);
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
void mc_ssp_payout(SSP_COMMAND *sspC, int amount, char *cc);
void mc_ssp_poll_device(struct m_device *device, struct m_metacash *metacash);
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char non_volatile);
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC, unsigned char *reason);
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC);

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash);
void mc_setup(struct m_metacash *metacash);

// cash hardware result parsing
void mc_handle_events_hopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

static const char *CURRENCY = "EUR";
static const char ROUTE_CASHBOX = 0x01;
static const char ROUTE_STORAGE = 0x00;
static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

int receivedSignal = 0;

void interrupt(int signal) {
	receivedSignal = signal;
}

redisAsyncContext* mc_connect_redis(struct m_metacash *metacash) {
	redisAsyncContext *conn = redisAsyncConnect(metacash->redisHost, metacash->redisPort);

	if(conn == NULL || conn->err) {
		if(conn) {
			fprintf(stderr, "fatal: Connection error: %s\n", conn->errstr);
		} else {
			fprintf(stderr, "fatal: Connection error: can't allocate redis context\n");
		}
	} else {
		// reference the metcash struct in data for use in connect/disconnect callback
		conn->data = metacash;
	}

	return conn;
}

void cbPollEvent(int fd, short event, void *privdata) {
	struct m_metacash *metacash = privdata;
	if(metacash->deviceAvailable == 0) {
		// return immediately if we have no actual hardware to poll
		return;
	}

	// cash hardware
	unsigned long amountBeforePoll = metacash->credit.amount;

	mc_ssp_poll_device(&metacash->hopper, metacash);
	mc_ssp_poll_device(&metacash->validator, metacash);

	if (metacash->credit.amount != amountBeforePoll) {
		printf("current credit now: %ld cents\n", metacash->credit.amount);
		// TODO: publish new amount of credit
	}
}

void cbCheckQuit(int fd, short event, void *privdata) {
	if(receivedSignal != 0) {
		syslog(LOG_NOTICE, "received signal. going to exit event loop.");

		struct m_metacash *metacash = privdata;
		event_base_loopexit(metacash->eventBase, NULL);
		receivedSignal = 0;
	}
}

void cbOnMetacashMessage(redisAsyncContext *c, void *r, void *privdata) {
	if(r == NULL) return;

	struct m_metacash *m = c->data;
	redisReply *reply = r;

	// example from http://stackoverflow.com/questions/16213676/hiredis-waiting-for-message
    if(reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
    	if(strcmp(reply->element[0]->str, "subscribe") != 0) {
    		char *topic = reply->element[1]->str;
    		char *message = reply->element[2]->str;

    		struct m_device *device = NULL;
        	redisAsyncContext *db = m->db;

    		if(strstr(message, "'dev':'validator'")) {
    			device = &m->validator;
    		} else if(strstr(message, "'dev':'hopper'")) {
    			device = &m->hopper;
    		} else {
    			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'error':'missing device in message'}");
    			printf("cbOnMetacashMessage: message missing device '%s'", message);
    			return;
    		}

            if(strstr(message, "'cmd':'empty'")) {
            	mc_ssp_empty(&device->sspC);
            } else if(strstr(message, "'cmd':'enable'")) {
            	ssp6_enable(&device->sspC);
            } else if(strstr(message, "'cmd':'disable'")) {
            	ssp6_disable(&device->sspC);
            } else if(strstr(message, "'cmd':'payout'")) {
            	char *amountToken = "'amount':'";
            	char *amountStart = strstr(message, amountToken);
            	if(amountStart != NULL) {
                	amountStart = amountStart + strlen(amountToken);
            		int amount = atoi(amountStart);

            		if (ssp6_payout(&device->sspC, amount, "EUR", SSP6_OPTION_BYTE_DO)
            				!= SSP_RESPONSE_OK) {
            			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
                    	redisAsyncContext *db = m->db;
            			switch (device->sspC.ResponseData[1]) {
            			case 0x01:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'payout','error':'not enough value in smart payout'}");
            				break;
            			case 0x02:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'payout','error':'can\'t pay exact amount'}");
            				break;
            			case 0x03:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'payout','error':'smart payout busy'}");
            				break;
            			case 0x04:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'payout','error':'smart payout disabled'}");
            				break;
            			default:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'payout','error':'unknown'}");
                			break;
            			}
            		}
            	}
            } else if(strstr(message, "'cmd':'test'")) {
            	char *amountToken = "'amount':'";
            	char *amountStart = strstr(message, amountToken);
            	if(amountStart != NULL) {
                	amountStart = amountStart + strlen(amountToken);
            		int amount = atoi(amountStart);

            		if (ssp6_payout(&device->sspC, amount, "EUR", SSP6_OPTION_BYTE_TEST)
            				!= SSP_RESPONSE_OK) {
            			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
                    	redisAsyncContext *db = m->db;
            			switch (device->sspC.ResponseData[1]) {
            			case 0x01:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','error':'not enough value in smart payout'}");
            				break;
            			case 0x02:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','error':'can\'t pay exact amount'}");
            				break;
            			case 0x03:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','error':'smart payout busy'}");
            				break;
            			case 0x04:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','error':'smart payout disabled'}");
            				break;
            			default:
                			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','error':'unknown'}");
                			break;
            			}
            		} else {
            			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'response':'test','result':'ok'}");
            		}
            	}
            } else if(strstr(message, "'cmd':'last reject note'")) {
            	unsigned char reasonCode;
            	char *reason = NULL;

            	if(mc_ssp_last_reject_note(&device->sspC, &reasonCode) == SSP_RESPONSE_OK) {
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
                	if(reason != NULL) {
                		printf("reason found: %s\n", reason);
                		char *response = NULL;
                		asprintf(&response, "{'response':'last reject note','reason':'%s',code:%ld}", reason, reasonCode);
            			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", response);
            			free(response);
                	} else {
                		printf("reason undefined\n");
                		char *response = NULL;
                		asprintf(&response, "{'response':'last reject note','reason':'undefined',code:%ld}", reasonCode);
            			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", response);
            			free(response);
                	}
            	} else {
            		printf("reason timeout\n");
        			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'timeout':'last reject note'}");
            	}
            } else {
    			redisAsyncCommand(db, NULL, NULL, "PUBLISH response %s", "{'error':'unable to process message'}");
            	printf("cbOnMetacashMessage: message missing cmd '%s'", message);
            }
    	}
    }
}

void cbConnectDb(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "Connected to database...\n");

	// redisCommand(c->c, "GET credit"); // TODO: read the current credit sync and store in metacash
	// redisAsyncCommand(db, NULL, NULL, "SET component:ssp 1");
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

	redisAsyncContext *cNotConst = (redisAsyncContext*) c; // get rids of discarding qualifier 'const' warning

	redisAsyncCommand(cNotConst, cbOnMetacashMessage, NULL, "SUBSCRIBE metacash");
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
	setlogmask(LOG_UPTO (LOG_NOTICE));
	openlog("metacashd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	syslog(LOG_NOTICE, "Program started by User %d", getuid());

	// register interrupt handler for signals
	signal(SIGTERM, interrupt);
	signal(SIGINT, interrupt);

	struct m_metacash metacash;
	metacash.deviceAvailable = 0;
	metacash.quit = 0;
	metacash.credit.amount = 0;
	metacash.serialDevice = "/dev/ttyACM0";	// default, override with -d argument
	metacash.redisHost = "127.0.0.1";		// default, override with -h argument
	metacash.redisPort = 6379;				// default, override with -p argument

	metacash.hopper.id = 0x10; // 0X10 -> Smart Hopper ("Münzer")
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.parsePoll = mc_handle_events_hopper;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.parsePoll = mc_handle_events_validator;

	// parse the command line arguments
	if (parseCmdLine(argc, argv, &metacash)) {
		return 1;
	}

	syslog(LOG_NOTICE, "using redis at %s:%d and hardware device %s",
			metacash.redisHost, metacash.redisPort, metacash.serialDevice);

	// open the serial device
	if (mc_ssp_open_serial_device(&metacash) == 0) {
		metacash.deviceAvailable = 1;
	} else {
		syslog(LOG_ALERT, "cash hardware unavailable");
	}

	// setup the ssp commands, configure and initialize the hardware
	mc_setup(&metacash);

	syslog(LOG_NOTICE, "metacash open for business :D");

	event_base_dispatch(metacash.eventBase); // blocking until exited via api-call

	syslog(LOG_NOTICE, "exiting");

	if(metacash.deviceAvailable) {
		mc_ssp_close_serial_device(&metacash);
	}

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
				fprintf(stderr, "Unknown option `-%c'.\n", optopt);
			else
				fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
			return 1;
		default:
			return 1;
		}

	return 0;
}

// business stuff
void mc_handle_events_hopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	redisAsyncContext *db = metacash->db;
	char *response = NULL;

	int i;
	for (i = 0; i < poll->event_count; ++i) {
		switch(poll->events[i].event) {
		case SSP_POLL_RESET:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'unit reset'}");
			// Make sure we are using ssp version 6
			if(ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the 'read' event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if(poll->events[i].data1 > 0) {
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'read','channel':%ld}",
						poll->events[i].data1);
			} else {
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'reading'}");
			}
			break;
		case SSP_POLL_DISPENSING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'dispensing','channel':%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_DISPENSED:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'dispensed','channel':%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_JAMMED:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'jammed'}");
			break;
		case SSP_POLL_COIN_CREDIT:
			metacash->credit.amount++; // hopper will report all coins as 1 cent m(
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'coin credit'}");
			break;
		case SSP_POLL_EMPTY:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'empty'}");
			break;
		case SSP_POLL_EMPTYING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'emptying'}");
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'credit','channel':%ld,'cc':'%s'}",
					poll->events[i].data1,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
    		// the validator shutdown during a payout, this event is reporting that some value remains to payout
    		asprintf(&response, "{'event':'incomplete payout','dispensed':%ld,'requested':%ld,'cc':'%s'}",
				poll->events[i].data1,
				poll->events[i].data2,
				poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", response);
			free(response);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
    		asprintf(&response, "{'event':'incomplete float','dispensed':%ld,'requested':%ld,'cc':'%s'}",
				poll->events[i].data1,
				poll->events[i].data2,
				poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", response);
			free(response);
			break;
		case SSP_POLL_DISABLED:
			// The unit has been disabled
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'disabled'}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch(poll->events[i].data1) {
			case NO_FAILUE:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'no error'}");
				break;
			case SENSOR_FLAP:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'sensor flap'}");
				break;
			case SENSOR_EXIT:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'sensor exit'}");
				break;
			case SENSOR_COIL1:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'sensor coil 1'}");
				break;
			case SENSOR_COIL2:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'sensor coil 2'}");
				break;
			case NOT_INITIALISED:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'not initialized'}");
				break;
			case CHECKSUM_ERROR:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper %s", "{'event':'calibration fail','error':'checksum error'}");
				break;
			case COMMAND_RECAL:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'recalibrating'}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH hopper {'event':'unknown','id':'0x%02X'}", poll->events[i].event);
			break;
		}
	}
}

void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	redisAsyncContext *db = metacash->db;
	char *response = NULL;

	for(int i = 0; i < poll->event_count; ++i) {
		switch(poll->events[i].event) {
		case SSP_POLL_RESET:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'unit reset'}");
			// Make sure we are using ssp version 6
			if(ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the 'read' event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if(poll->events[i].data1 > 0) {
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'read','channel':%ld}",
						poll->events[i].data1);
			} else {
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'reading'}");
			}
			break;
		case SSP_POLL_EMPTY:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'empty'}");
			break;
		case SSP_POLL_EMPTYING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'emptying'}");
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'credit','channel':%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
    		asprintf(&response, "{'event':'incomplete payout','dispensed':%ld,'requested':%ld,'cc':'%s'}",
				poll->events[i].data1,
				poll->events[i].data2,
				poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", response);
			free(response);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
    		asprintf(&response, "{'event':'incomplete float','dispensed':%ld,'requested':%ld,'cc':'%s'}",
				poll->events[i].data1,
				poll->events[i].data2,
				poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", response);
			free(response);
			break;
		case SSP_POLL_REJECTING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'rejecting'}");
			break;
		case SSP_POLL_REJECTED:
			// The note was rejected
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'rejected'}");
			break;
		case SSP_POLL_STACKING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stacking'}");
			break;
		case SSP_POLL_STORED:
			// The note has been stored in the payout unit
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stored'}");
			break;
		case SSP_POLL_STACKED:
			// The note has been stacked in the cashbox
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stacked'}");
			break;
		case SSP_POLL_SAFE_JAM:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'safe jam'}");
			break;
		case SSP_POLL_UNSAFE_JAM:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'unsafe jam'}");
			break;
		case SSP_POLL_DISABLED:
			// The validator has been disabled
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'disabled'}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			// The validator has detected a fraud attempt
    		asprintf(&response, "{'event':'fraud attempt','dispensed':%ld}",
				poll->events[i].data1);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", response);
			free(response);
			break;
		case SSP_POLL_STACKER_FULL:
			// The cashbox is full
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'stacker full'}");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			// The cashbox has been removed
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'cashbox removed'}");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			// The cashbox has been replaced
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'cashbox replaced'}");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			// A note was in the notepath at startup and has been cleared from the front of the validator
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'cleared from front'}");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			// A note was in the notepath at startup and has been cleared into the cashbox
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'cleared into cashbox'}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch(poll->events[i].data1) {
			case NO_FAILUE:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'no error'}");
				break;
			case SENSOR_FLAP:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'sensor flap'}");
				break;
			case SENSOR_EXIT:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'sensor exit'}");
				break;
			case SENSOR_COIL1:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'sensor coil 1'}");
				break;
			case SENSOR_COIL2:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'sensor coil 2'}");
				break;
			case NOT_INITIALISED:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'not initialized'}");
				break;
			case CHECKSUM_ERROR:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator %s", "{'event':'calibration fail','error':'checksum error'}");
				break;
			case COMMAND_RECAL:
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'recalibrating'}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'unknown','id':'0x%02X'}", poll->events[i].event);
			break;
		}
	}
}

void mc_setup(struct m_metacash *metacash) {
	// initialize libEvent
	metacash->eventBase = event_base_new();

	// connect to redis
	metacash->db = mc_connect_redis(metacash);		// establish connection for persistence
	metacash->pubSub = mc_connect_redis(metacash);	// establich connection for pub/sub

	// setup redis
	if(metacash->db && metacash->pubSub) {
		redisLibeventAttach(metacash->db, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->db, cbConnectDb);
		redisAsyncSetDisconnectCallback(metacash->db, cbDisconnectDb);

		redisLibeventAttach(metacash->pubSub, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->pubSub, cbConnectPubSub);
		redisAsyncSetDisconnectCallback(metacash->pubSub, cbDisconnectPubSub);
	}

	// try to initialize the hardware only if we successfully have opened the device
	if(metacash->deviceAvailable) {
		// prepare the device structures
		mc_ssp_setup_command(&metacash->validator.sspC, metacash->validator.id);
		mc_ssp_setup_command(&metacash->hopper.sspC, metacash->hopper.id);

		// initialize the devices
		printf("\n");
		mc_ssp_initialize_device(&metacash->validator.sspC,
				metacash->validator.key);
		printf("\n");
		mc_ssp_initialize_device(&metacash->hopper.sspC, metacash->hopper.key);
		printf("\n");

		// reject notes unfit for storage.
		// if this is not enabled, notes unfit for storage will be silently redirected
		// to the cashbox of the validator from which no payout can be done.
		if(mc_ssp_set_refill_mode(&metacash->validator.sspC) != SSP_RESPONSE_OK) {
			printf("ERROR: setting refill mode failed\n");
		}

		// setup the routing of the banknotes in the validator (amounts are in cent)
		ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY, ROUTE_CASHBOX); // 5 euro
		ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY, ROUTE_CASHBOX); // 10 euro
		ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY, ROUTE_STORAGE); // 20 euro
		ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY, ROUTE_STORAGE); // 50 euro
		ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY, ROUTE_STORAGE); // 100 euro
		ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY, ROUTE_STORAGE); // 200 euro
		ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY, ROUTE_STORAGE); // 500 euro
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

	// setup libevent triggered check if we should quit (every 200ms more or less)
	{
		struct timeval interval;
		interval.tv_sec = 0;
		interval.tv_usec = 200;

		event_set(&metacash->evCheckQuit, 0, EV_PERSIST, cbCheckQuit, metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evCheckQuit);
		evtimer_add(&metacash->evCheckQuit, &interval);
	}
}

int mc_ssp_open_serial_device(struct m_metacash *metacash) {
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

void mc_ssp_close_serial_device(struct m_metacash *metacash) {
	close_ssp_port();
}

void mc_ssp_poll_device(struct m_device *device, struct m_metacash *metacash) {
	SSP_POLL_DATA6 poll;

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
			printf("parsing poll response from '%s' now (%d events)\n",
					device->name, poll.event_count);
			device->parsePoll(device, metacash, &poll);
		} else {
			//printf("polling '%s' returned no events\n", device->name);
		}
	}
}

void mc_ssp_initialize_device(SSP_COMMAND *sspC, unsigned long long key) {
	SSP6_SETUP_REQUEST_DATA setup_req;
	unsigned int i = 0;

	printf("initializing device (id=0x%02X)\n", sspC->SSPAddress);

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
	if (ssp6_setup_request(sspC, &setup_req) != SSP_RESPONSE_OK) {
		printf("ERROR: Setup Request Failed\n");
		return;
	}

	printf("firmware: %s\n", setup_req.FirmwareVersion);
	printf("channels:\n");
	for (i = 0; i < setup_req.NumberOfChannels; i++) {
		printf("channel %d: %d %s\n", i + 1, setup_req.ChannelData[i].value,
				setup_req.ChannelData[i].cc);
	}

	//enable the device
	if (ssp6_enable(sspC) != SSP_RESPONSE_OK) {
		printf("ERROR: Enable Failed\n");
		return;
	}

	if (setup_req.UnitType == 0x03) {
		// SMART Hopper requires different inhibit commands
		for (i = 0; i < setup_req.NumberOfChannels; i++) {
			ssp6_set_coinmech_inhibits(sspC, setup_req.ChannelData[i].value,
					setup_req.ChannelData[i].cc, ENABLED);
		}
	} else {
		if (setup_req.UnitType == 0x06 || setup_req.UnitType == 0x07) {
			//enable the payout unit
			if (ssp6_enable_payout(sspC, setup_req.UnitType)
					!= SSP_RESPONSE_OK) {
				printf("ERROR: Enable Failed\n");
				return;
			}
		}

		// set the inhibits (enable all note acceptance)
		if (ssp6_set_inhibits(sspC, 0xFF, 0xFF) != SSP_RESPONSE_OK) {
			printf("ERROR: Inhibits Failed\n");
			return;
		}
	}

	printf("device has been successfully initialized\n");
}

void mc_ssp_setup_command(SSP_COMMAND *sspC, int deviceId) {
	sspC->SSPAddress = deviceId;
	sspC->Timeout = 1000;
	sspC->EncryptionStatus = NO_ENCRYPTION;
	sspC->RetryLevel = 3;
	sspC->BaudRate = 9600;
}

void mc_ssp_payout(SSP_COMMAND *sspC, int amount, char *cc) {
	if (amount > 0) {
		// send the test payout command
		if (ssp6_payout(sspC, amount, cc, SSP6_OPTION_BYTE_TEST)
				!= SSP_RESPONSE_OK) {

			printf("Test: Payout would fail");
			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
			switch (sspC->ResponseData[1]) {
			case 0x01:
				printf(": Not enough value in Smart Payout\n");
				break;
			case 0x02:
				printf(": Cant pay exact amount\n");
				break;
			case 0x03:
				printf(": Smart Payout Busy\n");
				break;
			case 0x04:
				printf(": Smart Payout Disabled\n");
				break;
			default:
				printf("\n");
			}

			return;
		}

		// send the payout command
		if (ssp6_payout(sspC, amount, cc, SSP6_OPTION_BYTE_DO)
				!= SSP_RESPONSE_OK) {

			printf("ERROR: Payout failed");
			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
			switch (sspC->ResponseData[1]) {
			case 0x01:
				printf(": Not enough value in Smart Payout\n");
				break;
			case 0x02:
				printf(": Cant pay exact amount\n");
				break;
			case 0x03:
				printf(": Smart Payout Busy\n");
				break;
			case 0x04:
				printf(": Smart Payout Disabled\n");
				break;
			default:
				printf("\n");
			}
		}
	}
}

SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC, unsigned char *reason) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = 0x17;

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
	sspC->CommandData[0] = 0x3;

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
	sspC->CommandData[0] = 0x4;

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
	sspC->CommandData[0] = 0x30;

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

SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char non_volatile) {
	sspC->CommandDataLength = 5;
	sspC->CommandData[0] = 0x54;
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
