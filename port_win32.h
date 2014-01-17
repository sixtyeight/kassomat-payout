#ifndef BASIC_DEMO6_WIN32_H
#define BASIC_DEMO6_WIN32_H

#ifdef INCLUDED_PORT
#error "Multiple ports included"
#endif
#define INCLUDED_PORT

#include <conio.h>
#include <Windows.h>
#include <stdio.h>

#include "ssp_helpers.h"

// windows does not provide usleep
#define usleep(x) Sleep(x/1000)

// linux port uses a changemode function but windows does not need this
#define changemode(x)

// some functions which are available under different names
#define kbhit _kbhit
#define getchar _getch



typedef UINT (CALLBACK* LPFNDLLFUNC1)(SSP_COMMAND* cmd);
LPFNDLLFUNC1 openPort;
typedef UINT (CALLBACK* LPFNDLLFUNC2)(void);
LPFNDLLFUNC2 closePort;
typedef UINT (CALLBACK* LPFNDLLFUNC3)(SSP_COMMAND* cmd,SSP_COMMAND_INFO* sspInfo);
LPFNDLLFUNC3 sspSendCommand;
typedef UINT (CALLBACK* LPFNDLLFUNC4)(SSP_KEYS* key, SSP_COMMAND* cmd);
LPFNDLLFUNC4 initiateSSPHostKeys;
typedef UINT (CALLBACK* LPFNDLLFUNC5)(SSP_KEYS* key);
LPFNDLLFUNC5 createSSPHostEncryptionKey;

static int open_ssp_port (SSP_COMMAND *sspC) {
	return openPort(sspC);
}
static void close_ssp_port () {
	closePort();
}

static int init_lib(void)
{
	HINSTANCE hinstLib;


	// Load DLL file
	hinstLib = LoadLibrary(TEXT("ITLSSPProc.dll"));
	if (hinstLib == NULL) {
		printf("ERROR: unable to load DLL\n");
		return 0;
	}

	openPort = (LPFNDLLFUNC1)GetProcAddress(hinstLib, "OpenSSPComPortUSB");//
	if (openPort == NULL) {
		printf("ERROR: unable to find DLL function OpenSSPComPort\n");
		FreeLibrary(hinstLib);
		return 0;
	}

	closePort = (LPFNDLLFUNC2)GetProcAddress(hinstLib, "CloseSSPComPortUSB");
	if (closePort == NULL) {
		printf("ERROR: unable to find DLL function CloseSSPComPort\n");
		FreeLibrary(hinstLib);
		return 0;
	}

	sspSendCommand = (LPFNDLLFUNC3)GetProcAddress(hinstLib, "SSPSendCommand");
	if (sspSendCommand == NULL) {
		printf("ERROR: unable to find DLL function SSPSendCommand\n");
		FreeLibrary(hinstLib);
		return 0;
	}
	initiateSSPHostKeys = (LPFNDLLFUNC4)GetProcAddress(hinstLib, "InitiateSSPHostKeys");
	if (initiateSSPHostKeys == NULL) {
		printf("ERROR: unable to find DLL function initiateSSPHostKeys\n");
		FreeLibrary(hinstLib);
		return 0;
	}
	createSSPHostEncryptionKey = (LPFNDLLFUNC5)GetProcAddress(hinstLib, "CreateSSPHostEncryptionKey");
	if (createSSPHostEncryptionKey == NULL) {
		printf("ERROR: unable to find DLL function createSSPHostEncryptionKey\n");
		FreeLibrary(hinstLib);
		return 0;
	}


	return 1;

}


static int SendSSPCommand(SSP_COMMAND* sp,SSP_COMMAND_INFO* si)
{
	int reTries = 0;
	int i;
	SSP_COMMAND ssp;

	
	/* take a copy for retries  */
	for(i = 0; i < sp->CommandDataLength; i++)
		ssp.CommandData[i] = sp->CommandData[i];  
	ssp.CommandDataLength = sp->CommandDataLength;

	while(reTries < sp->RetryLevel){ 	

		/* dll call  */
		//if(sspSendCommand(&sspC,&sspI) == 0)
		if(sspSendCommand(sp,si) == 0){
			sp->ResponseStatus = SSP_RESPONSE_TIMEOUT;
			return 0;
		}
		/* response OK  */
		if(sp->ResponseData[0] == SSP_RESPONSE_OK){
			sp->ResponseStatus = SSP_RESPONSE_OK;
			return 255;
		}
		
		/* retry for busy response  */
		if(sp->ResponseData[0] == SSP_RESPONSE_COMMAND_NOT_PROCESSED && sp->ResponseDataLength == 2 && sp->ResponseData[1] == 0x03 /*PAYOUT_BUSY*/){
			Sleep(500); /* delay for next retry */
			/* restore command data as it may have been changed by DLL encryption  */
			for(i = 0; i < sp->CommandDataLength; i++)
				sp->CommandData[i] = ssp.CommandData[i];  
			sp->CommandDataLength = ssp.CommandDataLength; 
		}

		/* Check for a payout error responses  */
		if(sp->ResponseData[0] == SSP_RESPONSE_COMMAND_NOT_PROCESSED && sp->ResponseDataLength == 2 && sp->ResponseData[1] == 0x01 /*NOT_ENOUGH_VALUE*/){
			sp->ResponseStatus = SSP_RESPONSE_COMMAND_NOT_PROCESSED;
			return 0x01;
		}
		if(sp->ResponseData[0] == SSP_RESPONSE_COMMAND_NOT_PROCESSED && sp->ResponseDataLength == 2 && sp->ResponseData[1] == 0x02 /*CANNOT_PAY_EXACT_AMOUNT*/){
			sp->ResponseStatus = SSP_RESPONSE_COMMAND_NOT_PROCESSED;
			return 0x02;
		}
		if(sp->ResponseData[0] == SSP_RESPONSE_COMMAND_NOT_PROCESSED && sp->ResponseDataLength == 2 && sp->ResponseData[1] == 0x04 /*PAYOUT_DISABLED*/){
			sp->ResponseStatus = SSP_RESPONSE_COMMAND_NOT_PROCESSED;
			return 0x04;
		}

		if(sp->ResponseData[0] == SSP_RESPONSE_KEY_NOT_SET){
			sp->ResponseStatus = SSP_RESPONSE_KEY_NOT_SET;
			return 0x01;
		}


			
		reTries++;
	}

	if(reTries >= sp->RetryLevel) {
		sp->ResponseStatus = SSP_RESPONSE_TIMEOUT;
		return 0;
	}

	sp->ResponseStatus = SSP_RESPONSE_OK;
	return 255;
}
static int send_ssp_command(SSP_COMMAND *sspC) {
	SSP_COMMAND_INFO si;
	return SendSSPCommand(sspC, &si);
}



static int negotiate_ssp_encryption(SSP_COMMAND *sspC, SSP_FULL_KEY *hostKey)
{
	SSP_KEYS sKey;
	int i;


	// DLL call 
	if(initiateSSPHostKeys(&sKey,sspC) == 0){
		printf("ERROR: Cannot Initiate host keys\n");
		return 0;
	}

	// send sync command to establish coms 
	sspC->EncryptionStatus = 0;
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_SYNC;
	if(send_ssp_command(sspC) == 0){
		printf("ERROR: Error sending sync command to slave\n");
		return 0;

	}



	// set generator command
	sspC->EncryptionStatus = 0;
	sspC->CommandDataLength = 9;
	sspC->CommandData[0] = SSP_CMD_SET_GENERATOR;
	for(i = 0; i < 8; i++)
		sspC->CommandData[i + 1] = (unsigned char)(sKey.Generator >> (i*8));
	if(send_ssp_command(sspC) == 0){
		printf("ERROR: Error sending GENERATOR command to slave\n");
		return 0;

	}
	// set modulus command
	sspC->EncryptionStatus = 0;
	sspC->CommandDataLength = 9;
	sspC->CommandData[0] = SSP_CMD_SET_MODULUS;
	for(i = 0; i < 8; i++)
		sspC->CommandData[i + 1] = (unsigned char)(sKey.Modulus >> (i*8));
	if(send_ssp_command(sspC) == 0){
		printf("ERROR: Error sending MODULUS command to slave\n");
		return 0;

	}
	// req key exchange command
	sspC->EncryptionStatus = 0;
	sspC->CommandDataLength = 9;
	sspC->CommandData[0] = SSP_CMD_KEY_EXCHANGE;
	for(i = 0; i < 8; i++)
		sspC->CommandData[i + 1] = (unsigned char)(sKey.HostInter  >> (i*8));
	if(send_ssp_command(sspC) == 0){
		printf("ERROR: Error sending KEY_EXCHANGE command to slave\n");
		return 0;

	}
	sKey.SlaveInterKey = 0;
	for(i = 0; i < 8; i++)
		sKey.SlaveInterKey += ((unsigned __int64)sspC->ResponseData[i + 1]) << (i*8);  


	// DLL call to create our key
	if(createSSPHostEncryptionKey(&sKey) == 0){
		printf("ERROR: Cannot Create host keys\n");
		return 0;
	}	

	hostKey->EncryptKey = sKey.KeyHost; 

	return 1;

}

/* DLL defs and references			*/
/*typedef UINT (CALLBACK* LPFNDLLFUNC1)(SSP_COMMAND* cmd);
LPFNDLLFUNC1 openPort;
typedef UINT (CALLBACK* LPFNDLLFUNC2)(void);
LPFNDLLFUNC2 closePort;

*/

#endif
