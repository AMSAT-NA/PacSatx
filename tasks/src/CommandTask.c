/*
 * Command.c
 *
 *  Created on: Mar 12, 2019
 *      Author: burns
 */
#include <pacsat.h>
#include "FreeRTOS.h"
#include "os_task.h"

//Flight software headers
#include "CommandTask.h"
#include "gpioDriver.h"
#include "serialDriver.h"
#include "het.h"
#include "gpioDriver.h"
#include "MET.h"
#include "nonvolManagement.h"
#include "UplinkCommands.h"
#include "TMS570Hardware.h"
#include "ao_fec_rx.h"
#include "aesdecipher.h"
#include "shadigest.h"
#include "canDriver.h"
#include "inet.h"
#include "ax5043_access.h"

#define command_print if(PrintCommandInfo)printf

extern bool InSafeMode,InScienceMode,InHealthMode;
extern bool JustReleasedFromBooster;
extern uint16_t NumWODSaved;
extern int32_t WODHkStoreIndex,WODSciStoreIndex;
extern rt1Errors_t localErrorCollection;

static uint8_t SWCmdCount,HWCmdCount;

/*
 * Forward Routines
 */
static void DecodeHardwareCommand(UplinkCommands);
static void DecodeSoftwareCommand(uint32_t);
static void TlmSWCommands(CommandAndArgs *comarg);
static void OpsSWCommands(CommandAndArgs *comarg);
static void DispatchSoftwareCommand(SWCmdUplink *uplink,bool local);
bool AuthenticateSoftwareCommand(SWCmdUplink *uplink);
bool CommandTimeOK(SWCmdUplink *uplink);


static bool PrintCommandInfo = false,CommandTimeEnabled = false;
static bool VUCIsDisabled=false;
static uint8_t UplinkCommandBuffer[SW_UPLINK_BUFFERS][SW_UPLINK_BITS]; // One byte per bit as required by fec code
uint8_t SWCmdRing[4] = {0,0,0,0};

//#define SPECIAL_RANDOM_NUMBER 49093  /* For little endian */
#define SPECIAL_RANDOM_NUMBER 50623 /* For big endian */

#define INSERT_IN_PAIR_RING_BUFFER(shortinfo,longinfo)\
        {\
    SWCmdRing[3] = SWCmdRing[2];\
    SWCmdRing[2] = SWCmdRing[1];\
    SWCmdRing[1] = SWCmdRing[0];\
    SWCmdRing[0] =(((shortinfo & 7)<< 5) | (longinfo & 0x1f));\
        }


void CommandTask(void *pvParameters)
{
    bool FallbackTimerActive = true;

    /*
     * This is a task which sits around waiting for a message either from the the hardware or the software
     * command decoder to tell it that a command has been received.  When that happens, it acts on the
     * command.
     *
     * It is also used to update time time-based items that can't be done from the MET module because
     * they must not be done from an interrupt routine.
     */


    //Intertask_Message message;
    vTaskSetApplicationTaskTag((xTaskHandle) 0, (pdTASK_HOOK_CODE)CommandWD );
    ReportToWatchdog(CommandWD);

    //RestartUplinkDecode(); // Initialize software command decoder
    SWCmdCount = 0;
    HWCmdCount = 0;

    InitInterTask(ToCommand, 10);
    InitEncryption();
    CommandTimeEnabled = ReadMRAMBoolState(StateCommandTimeCheck);
    while (1) {
        bool gotSomething;
        Intertask_Message msg;
        ReportToWatchdog(CommandWD);
        gotSomething=WaitInterTask(ToCommand,SECONDS(2), &msg);
        /*
         * Since this task is not terribly busy, we will also use it to trigger things that
         * run on a timer.
         */
        ReportToWatchdog(CommandWD);
        if(gotSomething && (msg.MsgType == CmdControlHasChanged)) {
            /*
             * We have switched control type.  If we are not in control, but were before then we want to
             * start up the command receiver
             */
#ifdef FOR_RECEIVE
                ax5043StartRx();
#else
                ax5043StopRx();
#endif

        } else if(gotSomething){
            /* Here we actually received a message, so there is a command */
            //GPIOSetOff(LED3); // THere was a command.  Tell a human for ground testing
            JustReleasedFromBooster = false;
            if(FallbackTimerActive){
                /*
                 * Remember we have received a command and turn off the fallback
                 * timer.
                 */
                WriteMRAMCommandReceived(true);
                FallbackTimerActive = false;
            }
            if(msg.MsgType == CmdTypeRawSoftware){
                DecodeSoftwareCommand(msg.argument);
            } else if(msg.MsgType == CmdTypeHardware){
                int fixedUp=0;
                //
                // The data lines are not attached to adjacent pins
                // in order so we have to straighten them out.
                if(msg.argument & 2){
                    fixedUp |= 8;
                }
                if(msg.argument & 1){
                    fixedUp |= 1;
                }
                DecodeHardwareCommand((UplinkCommands)fixedUp);
            }
        } else {
            // Just another timed thing not really related to this task
            //GPIOSetOn(LED3); // Turn the light off (so command light was on about 2 seconds)
        }
    }
}

/*******************************************************************
 * Here are some utility routines.
 *
 * Fake command is called from the debug task and from below.
 * CheckCommandTimeout is called from the command task.
 *
 ********************************************************************/
void SimulateHWCommand(UplinkCommands cmd){
    Intertask_Message msg;
    msg.MsgType = CmdTypeHardware;
    msg.argument = cmd;
    //debug_print("Fake command %d\n",cmd);
    NotifyInterTask(ToCommand,WATCHDOG_SHORT_WAIT_TIME,(Intertask_Message *)&msg);
}
void SimulateSwCommand(uint8_t namesp, uint16_t cmd, const uint16_t *args,int numargs){
    Intertask_Message msg;
    int i;
    msg.MsgType = CmdTypeValidatedSoftwareLocal;
    msg.argument = namesp;
    msg.argument2 = cmd;
    if(numargs > sizeof(msg.data))numargs=sizeof(msg.data);
    for(i=0;i<numargs;i++){
        msg.data[i] = args[i];
    }
    //debug_print("Fake command %d\n",cmd);
    NotifyInterTask(ToCommand,WATCHDOG_SHORT_WAIT_TIME,(Intertask_Message *)&msg);
}
static void DecodeHardwareCommand(UplinkCommands command){

    /*
     * This is where we decode a hardware command.  In other words, this is where
     * we get when the hardware device on the receiver finds a command and sends it
     * to the IHU.  No alt sequence decoding on Golf.
     */
    if(command & CMD_TX_OFF) {
        uint16_t args[1] = {1};
        command_print("Hw Cmd: Transmit off\n");
        SimulateSwCommand(SWCmdNSSpaceCraftOps,SWCmdOpsDCTTxInhibit,args,1); //Send to others

    }
}

void DispatchSoftwareCommand(SWCmdUplink *uplink,bool local){
    uint8_t nameSpace;

    /*
     * Ok, we have a command that has been error corrected, unwhitened, CRC checked, unconvolved, timestamped,
     * etc.
     *
     * Next we check other stuff that has to be right.  Specifically, we check the satellite address
     * and then authenticate, and also check the timestamp for something reasonable.
     */
    nameSpace = uplink->namespaceNumber;
    if(uplink->address != OUR_ADDRESS){
        command_print("Wrong address %x\n\r",uplink->address);
        return;
    }
    if(local && (nameSpace != SWCmdNSInternal)){
        //Enter in the telemetry ring buffer only if it originated on this
        //CPU (and don't report internal commands)
        INSERT_IN_PAIR_RING_BUFFER(nameSpace,uplink->comArg.command);
    }
    /*
     * If Command Time Checking was enabled and we got a command, we know it is working
     * so we can turn off the timeout function
     */
    if(CommandTimeEnabled)
        //SetInternalSchedules(NoTimeCommandTimeout,TIMEOUT_NONE);


    command_print("Namespace=%d,command=%d,arg=%d\n",nameSpace,uplink->comArg.command,
                  uplink->comArg.arguments[0]);

    switch(nameSpace){
    case SWCmdNSSpaceCraftOps: {
        OpsSWCommands(&uplink->comArg);
        break;
    }
    case SWCmdNSTelemetry:{
        TlmSWCommands(&uplink->comArg);
        break;
    }
    default:
        printf("Unknown namespace %d\n\r",nameSpace);
        localErrorCollection.DCTCmdFailNamespaceCnt++;
        return;

    }
}

/*
 * Just for initial pacsat code most of the operations that commands do are commented out.  We just
 * print the command so the uplink can be tested.
 */

void OpsSWCommands(CommandAndArgs *comarg){

    switch((int)comarg->command){

    // This first group is intended to write states into the MRAM

    case SWCmdOpsDisableAutosafe:
        command_print("Disable Autosafe Command\n\r");
        //DisableAutosafe();
        break;
    case SWCmdOpsEnableAutosafe:
        command_print("Enable Autosafe Command\n\r");
        //EnableAutosafe();
        break;
    case SWCmdOpsSetAutosafeVoltages:{
        uint16_t into=comarg->arguments[0],outof=comarg->arguments[1];
        command_print("Set autosafe voltages %d %d\n",into,outof);
        //SetAutosafe(into,outof);
        break;
    }

    //Now we get to other stuff

    case SWCmdOpsSafeMode:{
        command_print("Safe mode command\n");
        //SendSafeModeMsg(false); // False forced it to not be autosafe
        break;
    }
    case SWCmdOpsHealthMode:
        command_print("Health mode \n");
        //SendHealthModeMsg();
        break;
    case SWCmdOpsScienceMode:{
        int timeout = comarg->arguments[0];
        if(timeout<=0)timeout = 1; // Just in case
        //SendScienceModeMsg(timeout);
        break;
    }
    case SWCmdOpsClearMinMax:
        command_print("Clear minmax\n");
        //ClearMinMax();
        break;

    case SWCmdOpsEnableTransponder: {
        bool turnOn;
        turnOn = (comarg->arguments[0] != 0);
        if(turnOn){
            command_print("Enable transponder\n\r");

        } else {
            command_print("Disable transponder\n\r");
        }
        //EnableTransponder(turnOn);
        break;
    }
    case SWCmdOpsNoop:
        command_print("No-op command\n\r");
        break;


    case SWCmdOpsResetSpecified:{
            command_print("Reset this RT-IHU\n");
            WaitSystemWithWatchdog(CENTISECONDS(10));
            ProcessorReset();
        break;
    }
    case SWCmdOpsEnableCommandTimeCheck:{
        EnableCommandTimeCheck(comarg->arguments[0] != 0);
        if(CommandTimeEnabled){
            command_print("Enable command time check\n\r");

        } else {
            command_print("Disable command time check\n\r");
        }
        break;
    }
    case SWCmdOpsDCTTxInhibit:
        if(comarg->arguments[0] != 0) { // True means to inhibit it
            command_print("SW:Inhibit transmitting\n");
        } else {
            command_print("SW:Uninhibit transmitting\n");
        }
        break;
    case SWCmdOpsSelectDCTRFPower: {
        int myCpuIndex = 0;//ThisProcessorIsPrimary()?0:1;
        bool safePowerHigh = comarg->arguments[myCpuIndex]; // Arg 0 and 1 are for safe
        bool normalPowerHigh = comarg->arguments[myCpuIndex+2]; //Arg 2 and 3 are for normal
        command_print("Select Power Level; for this DCT, safe=%s,normal=%s\n",safePowerHigh?"high":"low",
                normalPowerHigh?"high":"low");
        //SetSafeRfPowerLevel(safePowerHigh);
        //SetNormalRfPowerLevel(normalPowerHigh);
#if 0
        WriteMRAMTelemLowPower();
        ax5043_set_frequency(freq);
        ax5043_set_power(power);
        normalModeHighPower=ReadMRAMBoolState(StateNormalRfPowerLevel);
#endif

        break;
    }
     default:
        localErrorCollection.DCTCmdFailCommandCnt++;
        command_print("Unknown Ops Command %d\n\r",comarg->command);
        break;


    }
}
void EnableCommandPrint(bool enable){
    PrintCommandInfo = enable;
}
void EnableCommandTimeCheck(bool enable){
    CommandTimeEnabled = enable;
    if(enable){
        //SetInternalSchedules(NoTimeCommandTimeout,SW_COMMAND_TIME_TIMEOUT);
    } else {
        //SetInternalSchedules(NoTimeCommandTimeout,TIMEOUT_NONE);
    }
    WriteMRAMBoolState(StateCommandTimeCheck,CommandTimeEnabled);
}
void TlmSWCommands(CommandAndArgs *comarg){
    switch(comarg->command){
    case SWCmdTlmWODSaveSize:
        command_print("Change WOD size to %d\n\r",comarg->arguments[0]);
        //ChangeWODSaved(comarg->arguments[0]);
        break;

    case SWCmdTlmLegacyGain:
        command_print("Tlm legacy gain to %d\n\r",comarg->arguments[0]);
        break;

    case SWCmdTlmDCTDrivePwr:{
        int myCpuIndex = 0;
        uint16_t lowPower = comarg->arguments[myCpuIndex]; // Arg 0 and 1 are for low
        uint16_t highPower = comarg->arguments[myCpuIndex+2]; //Arg 2 and 3 are for high
        command_print("Drive power reg for this proc are Low: %d, high: %d\n",lowPower,highPower);
        //SetDCTDriveValue(lowPower,highPower);
        break;
    }

    default:
        localErrorCollection.DCTCmdFailCommandCnt++;
        printf("Unknown Tlm Command\n\r");
        break;
    }
}

void DecodeSoftwareCommand(uint32_t bufferIndex){

    uint8_t UplinkCommandDecoded[SW_CMD_STRUCT_SIZE+SW_UPLINK_CRC]; //Leave extra space for CRC
    bool retVal;
    SWCmdUplink *softwareCommand = (SWCmdUplink *)&UplinkCommandDecoded;

    retVal = ao_fec_decode(&UplinkCommandBuffer[bufferIndex][0], SW_UPLINK_BITS, &UplinkCommandDecoded[0],
                           SW_CMD_STRUCT_SIZE+SW_UPLINK_CRC, (void *)0);
    if(retVal){
        /*
         * Some of these need to be fixed for authentication to work
         */


        if(AuthenticateSoftwareCommand(softwareCommand)){
            command_print("\n\rCommand Authenticated!\n");

            softwareCommand->comArg.arguments[0] = ttohs(softwareCommand->comArg.arguments[0]);
            softwareCommand->comArg.arguments[1] = ttohs(softwareCommand->comArg.arguments[1]);
            softwareCommand->comArg.arguments[2] = ttohs(softwareCommand->comArg.arguments[2]);
            softwareCommand->comArg.arguments[3] = ttohs(softwareCommand->comArg.arguments[3]);


            /*
             * Here we have a command that was received on the uplink and ready to act on.  Also send
             * it to the other processors
             */

            DispatchSoftwareCommand((SWCmdUplink *)UplinkCommandDecoded,true);
        } else {
            command_print("\n\rCommand does not authenticate\n");
        }
    } else {
        localErrorCollection.DCTCmdFailCRCCnt++;
        command_print("\n\rCRC does not match\n\r");
    }

}


bool AuthenticateSoftwareCommand(SWCmdUplink *uplink){
    uint32_t localSecureHash32[8],uplinkSecureHash32[8];
    int i;
    bool shaOK;
    SHA((uint8_t *)uplink, SW_COMMAND_SIZE, localSecureHash32);
    DeCipher32(&uplink->AuthenticationVector[0],(uint8_t *)uplinkSecureHash32);
    for(i=0;i<8;i++){
        uplinkSecureHash32[i] = ttohl(uplinkSecureHash32[i]);
    }
    shaOK = (memcmp(localSecureHash32,uplinkSecureHash32,32) == 0);
    if(shaOK){
        uplink->comArg.command = ttohs(uplink->comArg.command); // We might have to look to determine if authenticated
        return CommandTimeOK(uplink);
    } else {
        localErrorCollection.DCTCmdFailAuthenticateCnt++;
        return false;
    }

}
bool CommandTimeOK(SWCmdUplink *uplink){
    logicalTime_t timeNow;
    static int lastCommandTime = 0;
    int secondsOff,uplinkSeconds;
    uint16_t uplinkEpoch;
    bool goodTime = true;

    /*
     * Here we are checking the time included in the command to make sure it is within the appropriate
     * range of the satellite time.  This is to avoid replay attacks.
     *
     * The algorithm is as follows:
     *
     * 1) The reset epoch in the command must match the satellite exactly
     * 2) The seconds in the command MUST be greater than the seconds in the last command
     * 3) The seconds in the command has to be within a tolerance (specified in FoxConfig) of the satellite time
     */
    uplinkEpoch = ttohs(uplink->resetNumber);
    uplinkSeconds = ttoh24(uplink->secondsSinceReset);

    if(CommandTimeEnabled){
        /*
         * If command time is not enabled, always return true, i.e. it's ok.
         * Also as a safety valve, allow us to disable the time check if we can't seem to get it right by
         * putting in a specific random number as the 4th argument for the disable command
         */
        if((uplink->namespaceNumber == SWCmdNSSpaceCraftOps) &&
                (uplink->comArg.command == SWCmdOpsEnableCommandTimeCheck)  &&
                (uplink->comArg.arguments[3] == SPECIAL_RANDOM_NUMBER)
        ) return true;
        getTimestamp(&timeNow);
        secondsOff = uplinkSeconds - timeNow.METcount;
        command_print("UplinkTime,delta %d/%d,%d\n\r",uplink->resetNumber,uplink->secondsSinceReset,secondsOff);
        if(
                /*
                 * The reset epoch in the command HAS to match the satellite, and the satellite time must not
                 * be greater than the "expiration time" in the command.
                 *
                 * In addition, to avoid a reply attack, the command HAS to be newer than the last command received.
                 * And to avoid totally circumventing this whole check, the transmitted time can't be too far behind
                 * or too far ahead of the current satellite time.
                 */

                (uplinkEpoch != timeNow.IHUresetCnt)   || // The uplinked reset count is wrong
                (uplinkSeconds <= lastCommandTime)  || // The uplinked second time is less or same as that of last command
                (uplinkSeconds < timeNow.METcount) || // The uplinked command has expired
                (secondsOff < SECONDS_AUTHENTICATION_MAX_BEHIND) || // Uplinked time too far behind sat time
                (secondsOff > SECONDS_AUTHENTICATION_MAX_AHEAD)  // They uplink had an expiration time too far ahead
        ){
            goodTime = false;
            localErrorCollection.DCTCmdFailTimeCnt++;
        }
        if(goodTime)lastCommandTime = uplinkSeconds; // Do not allow the next command to have the same or earlier
    }
    return goodTime;
}


/*********************************************************************************
 * These routines are called from different tasks
 **********************************************************************************/

void incomingRawSoftwareCommand(uint8_t *packet) {
    /*
     * This routine is called from TelemetryRadio (and thus executes in that task's context)
     * when it receives an incoming raw software command packet.  This routines job
     * is to collect the packet and then send it in an intertask
     * message to the command task for decoding.
     */

    //    static uint8_t RFCommand[SW_UPLINK_BYTES];  // This is the actual message
    static uint8_t swCommandBufferIndex=0;          // And this is the index for the double-buffered message
    // sent to the command task.
    int i,bitNum=0,byteNum=0;


    Intertask_Message msg;
    command_print("Incoming RF software command received\n");

    for (i=0; i<SW_UPLINK_BITS; i++) {
        /*
         * Turn the single bits into the bytes that the FEC decode wants.
         */

        if (packet[byteNum] & 1<<(7-bitNum)) {
            UplinkCommandBuffer[0][i]=0;
        } else {
            UplinkCommandBuffer[0][i]=0xff;
        }

        if(++bitNum >=8){
            bitNum=0;
            byteNum++;
        }
    }

    msg.MsgType = CmdTypeRawSoftware;

    msg.argument = swCommandBufferIndex++; // If we allow several commands, this will be the array index to UplinkCommandBuffer
    if(swCommandBufferIndex >= SW_UPLINK_BUFFERS)swCommandBufferIndex = 0;

    ReportToWatchdog(CurrentTaskWD);
    NotifyInterTask(ToCommand,WATCHDOG_MAX_WAIT_TIME,(Intertask_Message *)&msg);
    ReportToWatchdog(CurrentTaskWD);

    return;

}

/*
 * Here are the timeout callbacks relating to commands
 */

void NoCommandTimeoutCallback(void){
    printf("No command timeout\n");
    SimulateSwCommand(SWCmdNSSpaceCraftOps,SWCmdOpsHealthMode,NULL,0);
    //SetInternalSchedules(NoCommandTimeout,TIMEOUT_NONE); // Set this timeout to never
}
void NoTimedSWCommandTimeoutCallback(void){
    printf("No command after setting time check\n");
    EnableCommandTimeCheck(false);
    //SetInternalSchedules(NoTimeCommandTimeout,TIMEOUT_NONE); // Set this timeout to never
}

/*
 * Here are some external calls
 */
uint8_t GetHWCmdCount(void){
    return HWCmdCount;
}
uint8_t GetSWCmdCount(void){
    return SWCmdCount;
}
bool GetVUCDisabled(void){
    return VUCIsDisabled;
}
