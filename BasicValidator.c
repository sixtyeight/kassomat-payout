#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "port_linux.h"
#include "ssp_defines.h"
#include "ssp_helpers.h"
#include "SSPComs.h"

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
	char *serialDevice;

	struct m_credit credit;

	struct m_device hopper;
	struct m_device validator;
};

struct m_network {
	char *port;
	int serverSocket;

	int clientSocket;
	int isClientConnected;
};

// mc_nw_* : metacash network functions (low level)

// thanks to Brian "Beej Jorgensen" Hall for "Beej's Guide to Network Programming - Using Internet Sockets"
// at http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html

int mc_nw_setup_server(struct m_network *network);
void mc_nw_connect_client(struct m_network *network);
void mc_nw_handle_client_command(struct m_network *network,
		struct m_metacash *metacash);
void mc_nw_send_client_message(struct m_network *network, char *message);
void mc_nw_send_client_welcome(struct m_network *network);

void sigchld_handler(int s);
void *get_in_addr(struct sockaddr *sa);

// mc_ssp_* : metacash ssp functions (cash hardware, low level)
int mc_ssp_open_serial_device(struct m_metacash *metacash);
void mc_ssp_close_serial_device(struct m_metacash *metacash);
void mc_ssp_setup_command(SSP_COMMAND *sspC, int deviceId);
void mc_ssp_initialize_device(SSP_COMMAND *sspC, unsigned long long key);
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
void mc_ssp_payout(SSP_COMMAND *sspC, int amount, char *cc);
void mc_ssp_poll_device(struct m_device *device, struct m_metacash *metacash);

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash,
		struct m_network *network);
void mc_setup(struct m_metacash *metacash);

// cash hardware result parsing
void mc_handle_events_hopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

// handling the telnet client commands
void mc_handle_cmd_enable(struct m_network *network, char *buf,
		struct m_metacash *metacash);
void mc_handle_cmd_disable(struct m_network *network, char *bufs,
		struct m_metacash *metacash);
void mc_handle_cmd_payout(struct m_network *network, char *buf,
		struct m_metacash *metacash);
void mc_handle_cmd_show_credit(struct m_network *network,
		struct m_metacash *metacash);
void mc_handle_cmd_clear_credit(struct m_network *network,
		struct m_metacash *metacash);
void mc_handle_cmd_empty(struct m_network *network, char *buf,
		struct m_metacash *metacash);
void mc_handle_cmd_quit(struct m_network *network);
void mc_handle_cmd_shutdown(struct m_network *network);

static const char *CURRENCY = "EUR";
static const char ROUTE_CASHBOX = 0x01;
static const char ROUTE_STORAGE = 0x00;
static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

int main(int argc, char *argv[]) {
	struct m_network network;
	network.port = "1337";
	network.isClientConnected = 0;

	struct m_metacash metacash;
	metacash.credit.amount = 0;
	metacash.serialDevice = "/dev/ttyACM0";

	metacash.hopper.id = 0x10; // 0X10 -> Smart Hopper ("Münzer")
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.parsePoll = mc_handle_events_hopper;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.parsePoll = mc_handle_events_validator;

	// parse the command line arguments
	if (parseCmdLine(argc, argv, &metacash, &network)) {
		return 1;
	}

	// open the serial device
	if (mc_ssp_open_serial_device(&metacash)) {
		return 1;
	}

	// setup the ssp commands, configure and initialize the hardware
	mc_setup(&metacash);

	// network stuff
	mc_nw_setup_server(&network);

	printf("metacash open for business :D\n\n");

	while (1) {
		// look for connection if we are unconnected
		if (!network.isClientConnected) {
			mc_nw_connect_client(&network);
		} else { // look for commands from the client
			mc_nw_handle_client_command(&network, &metacash);
		}

		// cash hardware
		unsigned long amount = metacash.credit.amount;

		mc_ssp_poll_device(&metacash.hopper, &metacash);
		mc_ssp_poll_device(&metacash.validator, &metacash);

		if (metacash.credit.amount != amount) {
			printf("current credit now: %ld cents\n", metacash.credit.amount);
		}
	}

	mc_ssp_close_serial_device(&metacash);

	return 0;
}

int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash,
		struct m_network *network) {
	opterr = 0;

	char c;
	while ((c = getopt(argc, argv, "p:d:")) != -1)
		switch (c) {
		case 'd':
			metacash->serialDevice = optarg;
			break;
		case 'p':
			network->port = optarg;
			break;
		case '?':
			if (optopt == 'c' || optopt == 'p')
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

void mc_nw_send_client_welcome(struct m_network *network) {
	mc_nw_send_client_message(network,
			"welcome to      _                      _     \n");
	mc_nw_send_client_message(network,
			" _ __ ___   ___| |_ __ _  ___ __ _ ___| |__  \n");
	mc_nw_send_client_message(network,
			"| '_ ` _ \\ / _ \\ __/ _` |/ __/ _` / __| '_ \\ \n");
	mc_nw_send_client_message(network,
			"| | | | | |  __/ || (_| | (_| (_| \\__ \\ | | |\n");
	mc_nw_send_client_message(network,
			"|_| |_| |_|\\___|\\__\\__,_|\\___\\__,_|___/_| |_|\n");
	mc_nw_send_client_message(network, "");
}

void mc_nw_handle_client_command(struct m_network *network,
		struct m_metacash *metacash) {
	char buf[512] = { 0 };
	int byte_count;

	byte_count = recv(network->clientSocket, buf, sizeof buf, 0);

	if (byte_count == 0) {
		printf("server: client disconnected\n");
		close(network->clientSocket);
		network->isClientConnected = 0;
	} else if (byte_count == -1) {
		// nothing sent from client
	} else {
		char *cmd = strtok(buf, " \n\r");

		if (cmd == 0) {
			mc_nw_send_client_message(network, "> ");
			return;
		}

		int i;
		for (i = 0; cmd[i]; i++) {
			cmd[i] = tolower(cmd[i]);
		}

		printf("server: received command: %s\n", cmd);

		if (strcmp(cmd, "empty") == 0) {
			mc_handle_cmd_empty(network, buf, metacash);
		} else if (strcmp(cmd, "payout") == 0) {
			mc_handle_cmd_payout(network, buf, metacash);
		} else if (strcmp(cmd, "enable") == 0) {
			mc_handle_cmd_enable(network, buf, metacash);
		} else if (strcmp(cmd, "disable") == 0) {
			mc_handle_cmd_disable(network, buf, metacash);
		} else if (strcmp(cmd, "show") == 0) {
			mc_handle_cmd_show_credit(network, metacash);
		} else if (strcmp(cmd, "clear") == 0) {
			mc_handle_cmd_clear_credit(network, metacash);
		} else if (strcmp(cmd, "quit") == 0) {
			mc_handle_cmd_quit(network);
		} else if (strcmp(cmd, "shutdown") == 0) {
			mc_handle_cmd_shutdown(network);
		} else {
			mc_nw_send_client_message(network, "unknown command: ");
			mc_nw_send_client_message(network, buf);
			mc_nw_send_client_message(network, "\n> ");
		}
	}
}

void mc_handle_cmd_payout(struct m_network *network, char *buf,
		struct m_metacash *metacash) {
	char *device = strtok(NULL, " ");
	char *amount = strtok(NULL, " ");

	if (strcmp(device, "c") == 0) {
		mc_ssp_payout(&metacash->hopper.sspC, atoi(amount), "EUR");
	} else if (strcmp(device, "n") == 0) {
		mc_ssp_payout(&metacash->validator.sspC, atoi(amount), "EUR");
	} else {
		mc_nw_send_client_message(network, "unknown device");
	}
	mc_nw_send_client_message(network, "\n> ");
}

void mc_handle_cmd_show_credit(struct m_network *network,
		struct m_metacash *metacash) {
	mc_nw_send_client_message(network, "current credit: ");
	char *c;

	asprintf(&c, "%ld cents\n> ", metacash->credit.amount);
	mc_nw_send_client_message(network, c);
	free(c);
}

void mc_handle_cmd_clear_credit(struct m_network *network,
		struct m_metacash *metacash) {
	metacash->credit.amount = 0;
	mc_handle_cmd_show_credit(network, metacash);
}

void mc_handle_cmd_enable(struct m_network *network, char *buf,
		struct m_metacash *metacash) {
	char *device = strtok(NULL, " \n\r");
	if (strcmp(device, "c") == 0) {
		ssp6_enable(&metacash->hopper.sspC);
	} else if (strcmp(device, "n") == 0) {
		ssp6_enable(&metacash->validator.sspC);
	} else {
		mc_nw_send_client_message(network, "unknown device");
	}
	mc_nw_send_client_message(network, "\n> ");
}

void mc_handle_cmd_disable(struct m_network *network, char *buf,
		struct m_metacash *metacash) {
	char *device = strtok(NULL, " \n\r");
	if (strcmp(device, "c") == 0) {
		ssp6_disable(&metacash->hopper.sspC);
	} else if (strcmp(device, "n") == 0) {
		ssp6_disable(&metacash->validator.sspC);
	} else {
		mc_nw_send_client_message(network, "unknown device");
	}
	mc_nw_send_client_message(network, "\n> ");
}

void mc_handle_cmd_empty(struct m_network *network, char *buf,
		struct m_metacash *metacash) {
	char *device = strtok(NULL, " \n\r");
	if (strcmp(device, "c") == 0) {
		mc_ssp_empty(&metacash->hopper.sspC);
	} else if (strcmp(device, "n") == 0) {
		mc_ssp_empty(&metacash->validator.sspC);
	} else {
		mc_nw_send_client_message(network, "unknown device");
	}
	mc_nw_send_client_message(network, "\n> ");
}

void mc_handle_cmd_quit(struct m_network *network) {
	mc_nw_send_client_message(network, "bye\n> ");
	close(network->clientSocket);
	network->isClientConnected = 0;
}

void mc_handle_cmd_shutdown(struct m_network *network) {
	mc_nw_send_client_message(network, "shutting down metacash\n");
	mc_handle_cmd_quit(network);
	exit(0);
}

void mc_nw_send_client_message(struct m_network *network, char *message) {
	send(network->clientSocket, message, strlen(message), 0);
}

void mc_nw_connect_client(struct m_network *network) {
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	char s[INET6_ADDRSTRLEN];

	sin_size = sizeof their_addr;
	int clientSocket = accept(network->serverSocket,
			(struct sockaddr *) &their_addr, &sin_size);
	if (clientSocket == -1) {
		if (errno != EWOULDBLOCK) {
			perror("accept");
		} else {
			// nobody tries to connect
		}
	} else {
		inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
		fcntl(clientSocket, F_SETFL, O_NONBLOCK);
		printf("server: got connection from %s\n", s);

		network->clientSocket = clientSocket;
		network->isClientConnected = 1;

		mc_nw_send_client_welcome(network);
		mc_nw_send_client_message(network, "\n> ");
	}
}
int mc_nw_setup_server(struct m_network *network) {
	struct addrinfo hints;
	struct addrinfo *servinfo;
	struct addrinfo *p;
	struct sigaction sa;
	int addrinfo;
	int yes = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((addrinfo = getaddrinfo(NULL, network->port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(addrinfo));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		if ((network->serverSocket = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(network->serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			perror("setsockopt");
			freeaddrinfo(servinfo); // all done with this structure
			return 2;
		}

		fcntl(network->serverSocket, F_SETFL, O_NONBLOCK);

		if (bind(network->serverSocket, p->ai_addr, p->ai_addrlen) == -1) {
			close(network->serverSocket);
			perror("server: bind");
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo); // all done with this structure

	if (p == NULL) {
		fprintf(stderr, "server: failed to bind\n");
		return 3;
	}

	if (listen(network->serverSocket, 1) == -1) { // 1 connection max
		perror("listen");
		return 4;
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		return 5;
	}

	printf("server: waiting for connections on port %s\n", network->port);

	return 0;
}

void sigchld_handler(int s) {
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*) sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*) sa)->sin6_addr);
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
		case SSP_POLL_DISPENSING:
			printf("Dispensing\n");
			break;
		case SSP_POLL_DISPENSED:
			printf("Dispensed\n");
			break;
		case SSP_POLL_READ:
			// the 'read' event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				printf("Note Read %ld\n", poll->events[i].data1);
			} else {
				printf("Note Read (still scanning)\n");
			}
			break;
		case SSP_POLL_CREDIT:
			printf("Credit: Note %ld %s\n", poll->events[i].data1,
					poll->events[i].cc);

			int euro = 0;
			switch (poll->events[i].data1) {
			case 1:
				euro = 5;
				break;
			case 2:
				euro = 10;
				break;
			case 3:
				euro = 20;
				break;
			case 4:
				euro = 50;
				break;
			case 5:
				euro = 100;
				break;
			case 6:
				euro = 200;
				break;
			case 7:
				euro = 500;
				break;
			default:
				break;
			}

			metacash->credit.amount += euro * 100;

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
		case SSP_POLL_REJECTING:
			printf("Note Rejecting\n");
			break;
		case SSP_POLL_REJECTED:
			printf("Note Rejected\n");
			break;
		case SSP_POLL_STACKING:
			printf("Stacking\n");
			break;
		case SSP_POLL_STORED:
			printf("Stored\n");
			break;
		case SSP_POLL_STACKED:
			printf("Stacked\n");
			break;
		case SSP_POLL_SAFE_JAM:
			printf("Safe Jam\n");
			break;
		case SSP_POLL_UNSAFE_JAM:
			printf("Unsafe Jam\n");
			break;
		case SSP_POLL_DISABLED:
			printf("DISABLED\n");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			printf("Fraud Attempt %ld %s\n", poll->events[i].data1,
					poll->events[i].cc);
			break;
		case SSP_POLL_STACKER_FULL:
			printf("Stacker Full\n");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			printf("Cashbox Removed\n");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			printf("Cashbox Replaced\n");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			printf("Cleared from front\n");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			printf("Cleared Into Cashbox\n");
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

void mc_setup(struct m_metacash *metacash) {
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
	ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY, ROUTE_STORAGE); // 5 euro
	ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY, ROUTE_STORAGE); // 10 euro
	ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY, ROUTE_STORAGE); // 20 euro
	ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY, ROUTE_CASHBOX); // 50 euro
	ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY, ROUTE_CASHBOX); // 100 euro
	ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY, ROUTE_CASHBOX); // 200 euro
	ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY, ROUTE_CASHBOX); // 500 euro
}

int mc_ssp_open_serial_device(struct m_metacash *metacash) {
	// open the serial device
	printf("opening serial device: %s\n", metacash->serialDevice);
	const char *port = "/dev/ttyACM0";
	if (open_ssp_port(port) == 0) {
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
				// The validator has responded with key not set, so we should try to negotiate one
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

	usleep(200000); // 200 ms delay between polls
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

