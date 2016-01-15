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

void cbOnTestTopicMessage(redisAsyncContext *c, void *reply, void *privdata) {
	printf("cbOnTestTopicMessage: received a message via test-topic\n");

	struct m_metacash *m = c->data;
	redisAsyncCommand(m->db, NULL, NULL, "INCR test-msg-counter");
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

    		if(strstr(message, "'dev':'validator'")) {
    			device = &m->validator;
    		} else if(strstr(message, "'dev':'hopper'")) {
    			device = &m->hopper;
    		} else {
    			printf("cbOnMetacashMessage: message missing device '%s'", message);
    			return;
    		}

            if(strstr(message, "'cmd':'empty'")) {
            	mc_ssp_empty(&device->sspC);
            } else if(strstr(message, "'cmd':'enable'")) {
            	ssp6_enable(&device->sspC);
            } else if(strstr(message, "cmd':'disable'")) {
            	ssp6_disable(&device->sspC);
            } else {
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

	redisAsyncCommand(cNotConst, cbOnTestTopicMessage, NULL, "SUBSCRIBE test-topic");
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
	int i;
	for (i = 0; i < poll->event_count; ++i) {
		printf("processing event #%03d (0x%02X): ", i, poll->events[i].event);

		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			printf("Unit Reset\n");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				printf("Host Protocol Failed\n");
				return;
			}
			break;
		case SSP_POLL_COIN_CREDIT:
			printf("Credit coin\n");
			metacash->credit.amount++; // hopper will report all coins as 1 cent m(
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			printf("Incomplete payout %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			printf("Incomplete float %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_DISPENSING:
			printf("Dispensing\n");
			break;
		case SSP_POLL_DISPENSED:
			printf("Dispensed\n");
			break;
		case SSP_POLL_DISABLED:
			printf("DISABLED\n");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			printf("Calibration fail: ");

			switch (poll->events[i].data1) {
			case NO_FAILUE:
				printf("No failure\n");
				break;
			case SENSOR_FLAP:
				printf("Optical sensor flap\n");
				break;
			case SENSOR_EXIT:
				printf("Optical sensor exit\n");
				break;
			case SENSOR_COIL1:
				printf("Coil sensor 1\n");
				break;
			case SENSOR_COIL2:
				printf("Coil sensor 2\n");
				break;
			case NOT_INITIALISED:
				printf("Unit not initialised\n");
				break;
			case CHECKSUM_ERROR:
				printf("Data checksum error\n");
				break;
			case COMMAND_RECAL:
				printf("Recalibration by command required\n");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			printf("unknown event\n");
			break;
		}
	}
}

void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	redisAsyncContext *db = metacash->db;

	for(int i = 0; i < poll->event_count; ++i) {
		switch(poll->events[i].event) {
		case SSP_POLL_RESET:
			printf("Unit Reset\n");
			// Make sure we are using ssp version 6
			if(ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				fprintf(stderr, "SSP Host Protocol Failed\n");
				exit(3);
			}
			break;
		case SSP_POLL_READ:
			// the 'read' event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if(poll->events[i].data1 > 0) {
				printf("Note Read %ld %s\n", poll->events[i].data1, poll->events[i].cc);
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'read'}");
			} else {
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'reading'}");
			}
			break;
		case SSP_POLL_EMPTY:
			printf("Empty");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'empty'}");
			break;
		case SSP_POLL_EMPTYING:
			printf("Emptying");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'emptying'}");
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			printf("Credit %ld %s\n", poll->events[i].data1, poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'credit','channel':%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			printf("Incomplete payout %ld of %ld %s\n", poll->events[i].data1, poll->events[i].data2, poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'incomplete payout','dispensed':%ld,'requested':%ld}", poll->events[i].data1, poll->events[i].data2);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			printf("Incomplete float %ld of %ld %s\n", poll->events[i].data1, poll->events[i].data2, poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'incomplete float','floated':%ld,'requested':%ld}", poll->events[i].data1, poll->events[i].data2);
			break;
		case SSP_POLL_REJECTING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'rejecting'}");
			break;
		case SSP_POLL_REJECTED:
			// The note was rejected
			printf("Note Rejected\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'rejected'}");
			break;
		case SSP_POLL_STACKING:
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stacking'}");
			break;
		case SSP_POLL_STORED:
			// The note has been stored in the payout unit
			printf("Stored\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stored'}");
			break;
		case SSP_POLL_STACKED:
			// The note has been stacked in the cashbox
			printf("Stacked\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stacked'}");
			break;
		case SSP_POLL_SAFE_JAM:
			printf("Safe Jam\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'safe jam'}");
			break;
		case SSP_POLL_UNSAFE_JAM:
			printf("Unsafe Jam\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'unsafe jam'}");
			break;
		case SSP_POLL_DISABLED:
			// The validator has been disabled
			printf("DISABLED\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'disabled'}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			// The validator has detected a fraud attempt
			printf("Fraud Attempt %ld %s\n", poll->events[i].data1, poll->events[i].cc);
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'fraud attempt','dispensed':%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_STACKER_FULL:
			// The cashbox is full
			printf("Stacker Full\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'stacker full'}");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			// The cashbox has been removed
			printf("Cashbox Removed\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'cashbox removed'}");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			// The cashbox has been replaced
			printf("Cashbox Replaced\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'cashbox replaced'}");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			// A note was in the notepath at startup and has been cleared from the front of the validator
			printf("Cleared from front\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'cleared from front'}");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			// A note was in the notepath at startup and has been cleared into the cashbox
			printf("Cleared Into Cashbox\n");
			redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'cleared into cashbox'}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			printf("Calibration fail: ");

			switch(poll->events[i].data1) {
			case NO_FAILUE:
				printf ("No failure\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'no error'}");
				break;
			case SENSOR_FLAP:
				printf ("Optical sensor flap\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'sensor flap'}");
				break;
			case SENSOR_EXIT:
				printf ("Optical sensor exit\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'sensor exit'}");
				break;
			case SENSOR_COIL1:
				printf ("Coil sensor 1\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'sensor coil 1'}");
				break;
			case SENSOR_COIL2:
				printf ("Coil sensor 2\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'sensor coil 2'}");
				break;
			case NOT_INITIALISED:
				printf ("Unit not initialised\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'not initialized'}");
				break;
			case CHECKSUM_ERROR:
				printf ("Data checksum error\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'calibration fail','error':'checksum error'}");
				break;
			case COMMAND_RECAL:
				printf ("Recalibration by command required\n");
				redisAsyncCommand(db, NULL, NULL, "PUBLISH validator {'event':'recalibrating'}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			printf ("Unknown event: event=0x%02X\n", poll->events[i].event);
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

		// setup the routing of the banknotes in the validator (amounts are in cent)
		ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY, ROUTE_CASHBOX); // 5 euro
		ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY, ROUTE_CASHBOX); // 10 euro
		ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY, ROUTE_CASHBOX); // 20 euro
		ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY, ROUTE_CASHBOX); // 50 euro
		ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY, ROUTE_CASHBOX); // 100 euro
		ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY, ROUTE_CASHBOX); // 200 euro
		ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY, ROUTE_CASHBOX); // 500 euro
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

SSP_RESPONSE_ENUM mc_ssp_last_reject_code(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = 0x17;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	switch (sspC->ResponseData[1]) {
		case 0x00: // Note accepted
		case 0x01: // Note length incorrect
		case 0x02: // Reject reason 2
		case 0x03: // Reject reason 3
		case 0x04: // Reject reason 4
		case 0x05: // Reject reason 5
		case 0x06: // Channel inhibited
		case 0x07: // Second note inserted
		case 0x08: // Reject reason 8
		case 0x09: // Note recognised in more than one channel
		case 0x0A: // Reject reason 10
		case 0x0B: // Note too long
		case 0x0C: // Reject reason 12
		case 0x0D: // Mechanism slow/stalled
		case 0x0E: // Strimming attempt detected
		case 0x0F: // Fraud channel reject
		case 0x10: // No notes inserted
		case 0x11: // Peak detect fail
		case 0x12: // Twisted note detected
		case 0x13: // Escrow time-out
		case 0x14: // Bar code scan fail
		case 0x15: // Rear sensor 2 fail
		case 0x16: // Slot fail 1
		case 0x17: // Slot fail 2
		case 0x18: // Lens over-sample
		case 0x19: // Width detect fail
		case 0x1A: // Short note detected
		case 0x1B: // Note payout
		case 0x1C: // Unable to stack note
		default: //
	}
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
