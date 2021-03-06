/************************************************************************
* Copyright (c) 2009-2011,  Microchip Technology Inc.
*
* Microchip licenses this software to you solely for use with Microchip
* products.  The software is owned by Microchip and its licensors, and
* is protected under applicable copyright laws.  All rights reserved.
*
* SOFTWARE IS PROVIDED "AS IS."  MICROCHIP EXPRESSLY DISCLAIMS ANY
* WARRANTY OF ANY KIND, WHETHER EXPRESS OR IMPLIED, INCLUDING BUT
* NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.  IN NO EVENT SHALL
* MICROCHIP BE LIABLE FOR ANY INCIDENTAL, SPECIAL, INDIRECT OR
* CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, HARM TO YOUR
* EQUIPMENT, COST OF PROCUREMENT OF SUBSTITUTE GOODS, TECHNOLOGY
* OR SERVICES, ANY CLAIMS BY THIRD PARTIES (INCLUDING BUT NOT LIMITED
* TO ANY DEFENSE THEREOF), ANY CLAIMS FOR INDEMNITY OR CONTRIBUTION,
* OR OTHER SIMILAR COSTS.
*
* To the fullest extent allowed by law, Microchip and its licensors
* liability shall not exceed the amount of fees, if any, that you
* have paid directly to Microchip to use this software.
*
* MICROCHIP PROVIDES THIS SOFTWARE CONDITIONALLY UPON YOUR ACCEPTANCE
* OF THESE TERMS.
*
* Author        Date        Ver   Comment
*************************************************************************
* E. Schlunder  2009/04/14  0.01  Initial code ported from VB app.
* T. Lawrence   2011/01/14  2.90  Initial implementation of USB version of this
*                                 bootloader application.
* F. Schlunder  2011/07/06  2.90a Small update to support importing of hex files
*                                 with "non-monotonic" line address ordering.
************************************************************************/

#include <QTextStream>
#include <QByteArray>
#include <QList>
#include <QTime>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QSettings>
#include <QtWidgets/QDesktopWidget>
#include <QtConcurrent/QtConcurrentRun>

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "Settings.h"

#include "../version.h"




//Surely the micro doesn't have a programmable memory region greater than 268 Megabytes...
//Value used for error checking device reponse values.
#define MAXIMUM_PROGRAMMABLE_MEMORY_SEGMENT_SIZE 0x0FFFFFFF

bool deviceFirmwareIsAtLeast101 = false;
Comm::ExtendedQueryInfo extendedBootInfo;

unsigned char DataBuffer[0x200];  // buffer for eeprom data
int N;
int NumTones;
int FreqSpacing;
int SampleTime;
int CycleTime;

int ADDR;

#define eemap_boot_mode 0
#define eemap_firmware_version_major 1
#define eemap_firmware_version_minor 2
#define eemap_startup_mode 3
#define eemap_output_mode 4
#define eemap_number_of_tones 5
#define eemap_number_of_samples 6

#define eemap_device_serial 8
#define eemap_radio_frequency 10

#define eemap_threshold 12
#define eemap_hysteresis 14

#define eemap_tone1 16
#define eemap_tone2 18
#define eemap_tone3 20
#define eemap_tone4 22
#define eemap_tone5 24
#define eemap_tone6 26

#define eemap_pattern_on 28
#define eemap_pattern_off 29
#define eemap_fade_on 30
#define eemap_fade_off 31

#define eemap_group_address1 32
#define eemap_group_address2 34
#define eemap_group_address3 36
#define eemap_group_address4 38
#define eemap_group_address5 40
#define eemap_group_address6 42

#define eemap_antenna_type_address 44
#define eemap_serial_mode 45
#define eemap_save_station 46
#define bitmap 48

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindowClass)
{
    int i;
    hexOpen = false;
    fileWatcher = NULL;
    timer = new QTimer();

    ui->setupUi(this);
    setWindowTitle(APPLICATION + QString(" EEPROM Editor ") + VERSION);

    QSettings settings;
    settings.beginGroup("MainWindow");
    fileName = settings.value("fileName").toString();

    for(i = 0; i < MAX_RECENT_FILES; i++)
    {
        recentFiles[i] = new QAction(this);
        connect(recentFiles[i], SIGNAL(triggered()), this, SLOT(openRecentFile()));
        recentFiles[i]->setVisible(false);
        ui->menuFile->insertAction(ui->actionExit, recentFiles[i]);
    }
    ui->menuFile->insertSeparator(ui->actionExit);

    settings.endGroup();

    settings.beginGroup("WriteOptions");
    writeFlash = settings.value("writeFlash", false).toBool();
    //writeConfig = settings.value("writeConfig", false).toBool();
    writeConfig = false; //Force user to manually re-enable it every time they re-launch the application.  Safer that way.
    writeEeprom = settings.value("writeEeprom", true).toBool();
    eraseDuringWrite = true;
    settings.endGroup();

    comm = new Comm();
    deviceData = new DeviceData();
    hexData = new DeviceData();

    device = new Device(deviceData);

    qRegisterMetaType<Comm::ErrorCode>("Comm::ErrorCode");

    connect(timer, SIGNAL(timeout()), this, SLOT(Connection()));
    connect(this, SIGNAL(IoWithDeviceCompleted(QString,Comm::ErrorCode,double)), this, SLOT(IoWithDeviceComplete(QString,Comm::ErrorCode,double)));
    connect(this, SIGNAL(IoWithDeviceStarted(QString)), this, SLOT(IoWithDeviceStart(QString)));
    connect(this, SIGNAL(AppendString(QString)), this, SLOT(AppendStringToTextbox(QString)));
    //connect(this, SIGNAL(SetProgressBar(int)), this, SLOT(UpdateProgressBar(int)));
    //connect(comm, SIGNAL(SetProgressBar(int)), this, SLOT(UpdateProgressBar(int)));


    this->statusBar()->addPermanentWidget(&deviceLabel);
    deviceLabel.setText("Disconnected");

    //Make initial check to see if the USB device is attached
    comm->PollUSB();
    if(comm->isConnected())
    {
        qWarning("Attempting to open device...");
        comm->open();
        ui->plainTextEdit->setPlainText("Device Attached.");
        ui->plainTextEdit->appendPlainText("Connecting...");
        GetQuery();
    }
    else
    {
        ui->plainTextEdit->appendPlainText("Device not detected.  Short programming pins and cycle power ");
        deviceLabel.setText("Disconnected");
        hexOpen = false;
        setBootloadEnabled(false);
        //emit SetProgressBar(0);
    }

    //Update the file list in the File-->[import files list] area, so the user can quickly re-load a previously used .hex file.
    UpdateRecentFileList();

    timer->start(1000); //Check for future USB connection status changes every 1000 milliseconds.
}

MainWindow::~MainWindow()
{
    QSettings settings;

    settings.beginGroup("MainWindow");
    //settings.setValue("size", size());
    //settings.setValue("pos", pos());
    settings.setValue("fileName", fileName);
    settings.endGroup();

    settings.beginGroup("WriteOptions");
    settings.setValue("writeFlash", writeFlash);
    settings.setValue("writeConfig", writeConfig);
    settings.setValue("writeEeprom", writeEeprom);
    settings.endGroup();

    comm->close();
    setBootloadEnabled(false);

    delete timer;
    delete ui;
    delete comm;
    delete deviceData;
    delete hexData;
    delete device;
}

void MainWindow::Connection(void)
{
    bool currStatus = comm->isConnected();
    Comm::ErrorCode result;

    comm->PollUSB();

    if(currStatus != comm->isConnected())
    {
        UpdateRecentFileList();

        if(comm->isConnected())
        {
            qWarning("Attempting to open device...");
            comm->open();
            ui->plainTextEdit->setPlainText("Device Attached.");
            ui->plainTextEdit->appendPlainText("Connecting...");
            GetQuery();
        }
        else
        {
            qWarning("Closing device.");
            comm->close();
            deviceLabel.setText("Disconnected");
            ui->plainTextEdit->setPlainText("Device Detached.");
            hexOpen = false;
            setBootloadEnabled(false);
            //emit SetProgressBar(0);
        }
    }
}

void MainWindow::setBootloadEnabled(bool enable)
{
    ui->action_Settings->setEnabled(enable);
    ui->actionErase_Device->setEnabled(enable && !writeConfig);
    ui->actionWrite_Device->setEnabled(enable && hexOpen);
    ui->actionExit->setEnabled(enable);
    ui->action_Verify_Device->setEnabled(enable && hexOpen);
    ui->actionOpen->setEnabled(enable);
    ui->actionBlank_Check->setEnabled(enable && !writeConfig);
    ui->actionReset_Device->setEnabled(enable);
    ui->ReadEEPROM->setEnabled(enable);
    ui->WriteEEPROM->setEnabled(enable);
}

void MainWindow::setBootloadBusy(bool busy)
{
    if(busy)
    {
        QApplication::setOverrideCursor(Qt::BusyCursor);
        timer->stop();
    }
    else
    {
        QApplication::restoreOverrideCursor();
        timer->start(1000);
    }

    ui->action_Settings->setEnabled(!busy);
    ui->actionErase_Device->setEnabled(!busy && !writeConfig);
    ui->actionWrite_Device->setEnabled(!busy && hexOpen);
    ui->actionExit->setEnabled(!busy);
    ui->action_Verify_Device->setEnabled(!busy && hexOpen);
    ui->actionOpen->setEnabled(!busy);
    ui->action_Settings->setEnabled(!busy);
    ui->actionBlank_Check->setEnabled(!busy && !writeConfig);
    ui->actionReset_Device->setEnabled(!busy);
}

void MainWindow::on_actionExit_triggered()
{
    QApplication::exit();
}

void MainWindow::IoWithDeviceStart(QString msg)
{
    ui->plainTextEdit->appendPlainText(msg);
    setBootloadBusy(true);
}


//Useful for adding lines of text to the main window from other threads.
void MainWindow::AppendStringToTextbox(QString msg)
{
    ui->plainTextEdit->appendPlainText(msg);
}

//void MainWindow::UpdateProgressBar(int newValue)
//{
    //ui->progressBar->setValue(newValue);
//}



void MainWindow::IoWithDeviceComplete(QString msg, Comm::ErrorCode result, double time)
{
    QTextStream ss(&msg);

    switch(result)
    {
        case Comm::Success:
            ss << " Complete (" << time << "s)\n";
            setBootloadBusy(false);
            break;
        case Comm::NotConnected:
            ss << " Failed. Device not connected.\n";
            setBootloadBusy(false);
            break;
        case Comm::Fail:
            ss << " Failed.\n";
            setBootloadBusy(false);
            break;
        case Comm::IncorrectCommand:
            ss << " Failed. Unable to communicate with device.\n";
            setBootloadBusy(false);
            break;
        case Comm::Timeout:
            ss << " Timed out waiting for device (" << time << "s)\n";
            setBootloadBusy(false);
            break;
        default:
            break;
    }

    ui->plainTextEdit->appendPlainText(msg);
}

void MainWindow::on_action_Verify_Device_triggered()
{
    future = QtConcurrent::run(this, &MainWindow::VerifyDevice);
}


//Routine that verifies the contents of the non-voltaile memory regions in the device, after an erase/programming cycle.
//This function requests the memory contents of the device, then compares it against the parsed .hex file data to make sure
//The locations that got programmed properly match.
void MainWindow::VerifyDevice()
{
    Comm::ErrorCode result;
    DeviceData::MemoryRange deviceRange, hexRange;
    QTime elapsed;
    QString eeMsg;
    QTextStream ee(&eeMsg);

    unsigned int i, j;
    unsigned int arrayIndex;
    bool failureDetected = false;
    unsigned char flashData[MAX_ERASE_BLOCK_SIZE];
    unsigned char hexEraseBlockData[MAX_ERASE_BLOCK_SIZE];
    uint32_t startOfEraseBlock;
    uint32_t errorAddress = 0;
    uint16_t expectedResult = 0;
    uint16_t actualResult = 0;

    //Initialize an erase block sized buffer with 0xFF.
    //Used later for post SIGN_FLASH verify operation.
    memset(&hexEraseBlockData[0], 0xFF, MAX_ERASE_BLOCK_SIZE);

    emit IoWithDeviceStarted("Verifying Device...");
    foreach(deviceRange, deviceData->ranges)
    {
        if(writeFlash && (deviceRange.type == PROGRAM_MEMORY))
        {
            elapsed.start();
            //emit IoWithDeviceStarted("Verifying Device's Program Memory...");

            result = comm->GetData(deviceRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressFLASH,
                                   device->bytesPerWordFLASH,
                                   deviceRange.end,
                                   deviceRange.pDataBuffer);

            if(result != Comm::Success)
            {
                failureDetected = true;
                qWarning("Error reading device.");
                //emit IoWithDeviceCompleted("Verifying Device's Program Memory", result, ((double)elapsed.elapsed()) / 1000);
            }

            //Search through all of the programmable memory regions from the parsed .hex file data.
            //For each of the programmable memory regions found, if the region also overlaps a region
            //that was included in the device programmed area (which just got read back with GetData()),
            //then verify both the parsed hex contents and read back data match.
            foreach(hexRange, hexData->ranges)
            {
                if(deviceRange.start == hexRange.start)
                {
                    //For this entire programmable memory address range, check to see if the data read from the device exactly
                    //matches what was in the hex file.
                    for(i = deviceRange.start; i < deviceRange.end; i++)
                    {
                        //For each byte of each device address (1 on PIC18, 2 on PIC24, since flash memory is 16-bit WORD array)
                        for(j = 0; j < device->bytesPerAddressFLASH; j++)
                        {
                            //Check if the device response data matches the data we parsed from the original input .hex file.
                            if(deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j] != hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j])
                            {
                                //A mismatch was detected.

                                //Check if this is a PIC24 device and we are looking at the "phantom byte"
                                //(upper byte [j = 1] of odd address [i%2 == 1] 16-bit flash words).  If the hex data doesn't match
                                //the device (which should be = 0x00 for these locations), this isn't a real verify
                                //failure, since value is a don't care anyway.  This could occur if the hex file imported
                                //doesn't contain all locations, and we "filled" the region with pure 0xFFFFFFFF, instead of 0x00FFFFFF
                                //when parsing the hex file.
                                if((device->family == Device::PIC24) && ((i % 2) == 1) && (j == 1))
                                {
                                    //Not a real verify failure, phantom byte is unimplemented and is a don't care.
                                }
                                else
                                {
                                    //If the data wasn't a match, and this wasn't a PIC24 phantom byte, then if we get
                                    //here this means we found a true verify failure.
                                    failureDetected = true;
                                    if(device->family == Device::PIC24)
                                    {
                                        qWarning("Device: 0x%x Hex: 0x%x", *(uint16_t*)&deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j], *(uint16_t*)&hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j]);
                                    }
                                    else
                                    {
                                        qWarning("Device: 0x%x Hex: 0x%x", deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j], hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j]);
                                    }
                                    qWarning("Failed verify at address 0x%x", i);
                                    emit IoWithDeviceCompleted("Verify", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
                                    return;
                                }
                            }//if(deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j] != hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j])
                        }//for(j = 0; j < device->bytesPerAddressFLASH; j++)
                    }//for(i = deviceRange.start; i < deviceRange.end; i++)
                }//if(deviceRange.start == hexRange.start)
            }//foreach(hexRange, hexData->ranges)
            //emit IoWithDeviceCompleted("Verify", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        }//if(writeFlash && (deviceRange.type == PROGRAM_MEMORY))
        else if(writeEeprom && (deviceRange.type == EEPROM_MEMORY))
        {
            elapsed.start();
            //emit IoWithDeviceStarted("Verifying Device's EEPROM Memory...");
            result = comm->GetData(deviceRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressEEPROM,
                                   device->bytesPerWordEEPROM,
                                   deviceRange.end,
                                   deviceRange.pDataBuffer);

            if(result != Comm::Success)
            {
                failureDetected = true;
                qWarning("Error reading device.");
                //emit IoWithDeviceCompleted("Verifying Device's EEPROM Memory", result, ((double)elapsed.elapsed()) / 1000);
            }


            //Search through all of the programmable memory regions from the parsed .hex file data.
            //For each of the programmable memory regions found, if the region also overlaps a region
            //that was included in the device programmed area (which just got read back with GetData()),
            //then verify both the parsed hex contents and read back data match.
            foreach(hexRange, hexData->ranges)
            {
                if(deviceRange.start == hexRange.start)
                {
                    //For this entire programmable memory address range, check to see if the data read from the device exactly
                    //matches what was in the hex file.
                    for(i = deviceRange.start; i < deviceRange.end; i++)
                    {
                        //For each byte of each device address (only 1 for EEPROM byte arrays, presumably 2 for EEPROM WORD arrays)
                        for(j = 0; j < device->bytesPerAddressEEPROM; j++)
                        {
                            //Check if the device response data matches the data we parsed from the original input .hex file.
                            if(deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressEEPROM)+j] != hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressEEPROM)+j])
                            {
                                //A mismatch was detected.
                                failureDetected = true;
                                qWarning("Device: 0x%x Hex: 0x%x", deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j], hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressFLASH)+j]);
                                qWarning("Failed verify at address 0x%x", i);
                                emit IoWithDeviceCompleted("Verify EEPROM Memory", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
                                return;
                            }
                        }
                    }
                }
            }//foreach(hexRange, hexData->ranges)
            //emit IoWithDeviceCompleted("Verifying", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        }//else if(writeEeprom && (deviceRange.type == EEPROM_MEMORY))
        else if(writeConfig && (deviceRange.type == CONFIG_MEMORY))
        {
            elapsed.start();
            //emit IoWithDeviceStarted("Verifying Device's Config Words...");

            result = comm->GetData(deviceRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressConfig,
                                   device->bytesPerWordConfig,
                                   deviceRange.end,
                                   deviceRange.pDataBuffer);

            if(result != Comm::Success)
            {
                failureDetected = true;
                qWarning("Error reading device.");
                //emit IoWithDeviceCompleted("Verifying Device's Config Words", result, ((double)elapsed.elapsed()) / 1000);
            }

            //Search through all of the programmable memory regions from the parsed .hex file data.
            //For each of the programmable memory regions found, if the region also overlaps a region
            //that was included in the device programmed area (which just got read back with GetData()),
            //then verify both the parsed hex contents and read back data match.
            foreach(hexRange, hexData->ranges)
            {
                if(deviceRange.start == hexRange.start)
                {
                    //For this entire programmable memory address range, check to see if the data read from the device exactly
                    //matches what was in the hex file.
                    for(i = deviceRange.start; i < deviceRange.end; i++)
                    {
                        //For each byte of each device address (1 on PIC18, 2 on PIC24, since flash memory is 16-bit WORD array)
                        for(j = 0; j < device->bytesPerAddressConfig; j++)
                        {
                            //Compute an index into the device and hex data arrays, based on the current i and j values.
                            arrayIndex = ((i - deviceRange.start) * device->bytesPerAddressConfig)+j;

                            //Check if the device response data matches the data we parsed from the original input .hex file.
                            if(deviceRange.pDataBuffer[arrayIndex] != hexRange.pDataBuffer[arrayIndex])
                            {
                                //A mismatch was detected.  Perform additional checks to make sure it was a real/unexpected verify failure.

                                //Check if this is a PIC24 device and we are looking at the "phantom byte"
                                //(upper byte [j = 1] of odd address [i%2 == 1] 16-bit flash words).  If the hex data doesn't match
                                //the device (which should be = 0x00 for these locations), this isn't a real verify
                                //failure, since value is a don't care anyway.  This could occur if the hex file imported
                                //doesn't contain all locations, and we "filled" the region with pure 0xFFFFFFFF, instead of 0x00FFFFFF
                                //when parsing the hex file.
                                if((device->family == Device::PIC24) && ((i % 2) == 1) && (j == 1))
                                {
                                    //Not a real verify failure, phantom byte is unimplemented and is a don't care.
                                }//Make further special checks for PIC18 non-J devices
                                else if((device->family == Device::PIC18) && (deviceRange.start == 0x300000) && ((i == 0x300004) || (i == 0x300007)))
                                {
                                     //The "CONFIG3L" and "CONFIG4H" locations (0x300004 and 0x300007) on PIC18 non-J USB devices
                                     //are unimplemented and should be masked out from the verify operation.
                                }
                                else
                                {
                                    //If the data wasn't a match, and this wasn't a PIC24 phantom byte, then if we get
                                    //here this means we found a true verify failure.
                                    failureDetected = true;
                                    if(device->family == Device::PIC24)
                                    {
                                        qWarning("Device: 0x%x Hex: 0x%x", *(uint16_t*)&deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressConfig)+j], *(uint16_t*)&hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressConfig)+j]);
                                    }
                                    else
                                    {
                                        qWarning("Device: 0x%x Hex: 0x%x", deviceRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressConfig)+j], hexRange.pDataBuffer[((i - deviceRange.start) * device->bytesPerAddressConfig)+j]);
                                    }
                                    qWarning("Failed verify at address 0x%x", i);
                                    emit IoWithDeviceCompleted("Verify Config Bit Memory", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
                                    return;
                                }
                            }
                        }
                    }
                }
            }//foreach(hexRange, hexData->ranges)
            //emit IoWithDeviceCompleted("Verifying", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        }//else if(writeConfig && (deviceRange.type == CONFIG_MEMORY))
        else
        {
            continue;
        }
    }//foreach(deviceRange, deviceData->ranges)

    if(failureDetected == false)
    {
        //Successfully verified all regions without error.
        //If this is a v1.01 or later device, we now need to issue the SIGN_FLASH
        //command, and then re-verify the first erase page worth of flash memory
        //(but with the exclusion of the signature WORD address from the verify,
        //since the bootloader firmware will have changed it to the new/magic
        //value (probably 0x600D, or "good" in leet speak).
        if(deviceFirmwareIsAtLeast101 == true)
        {
            comm->SignFlash();

            qDebug("Expected Signature Address: 0x%x", extendedBootInfo.PIC18.signatureAddress);
            qDebug("Expected Signature Value: 0x%x", extendedBootInfo.PIC18.signatureValue);


            //Now re-verify the first erase page of flash memory.
            if(device->family == Device::PIC18)
            {
                startOfEraseBlock = extendedBootInfo.PIC18.signatureAddress - (extendedBootInfo.PIC18.signatureAddress % extendedBootInfo.PIC18.erasePageSize);
                result = comm->GetData(startOfEraseBlock,
                                       device->bytesPerPacket,
                                       device->bytesPerAddressFLASH,
                                       device->bytesPerWordFLASH,
                                       (startOfEraseBlock + extendedBootInfo.PIC18.erasePageSize),
                                       &flashData[0]);
                if(result != Comm::Success)
                {
                    failureDetected = true;
                    qWarning("Error reading, post signing, flash data block.");
                }

                //Search through all of the programmable memory regions from the parsed .hex file data.
                //For each of the programmable memory regions found, if the region also overlaps a region
                //that is part of the erase block, copy out bytes into the hexEraseBlockData[] buffer,
                //for re-verification.
                foreach(hexRange, hexData->ranges)
                {
                    //Check if any portion of the range is within the erase block of interest in the device.
                    if((hexRange.start <= startOfEraseBlock) && (hexRange.end > startOfEraseBlock))
                    {
                        unsigned int rangeSize = hexRange.end - hexRange.start;
                        unsigned int address = hexRange.start;
                        unsigned int k = 0;

                        //Check every byte in the hex file range, to see if it is inside the erase block of interest
                        for(i = 0; i < rangeSize; i++)
                        {
                            //Check if the current byte we are looking at is inside the erase block of interst
                            if(((address+i) >= startOfEraseBlock) && ((address+i) < (startOfEraseBlock + extendedBootInfo.PIC18.erasePageSize)))
                            {
                                //The byte is in the erase block of interst.  Copy it out into a new buffer.
                                hexEraseBlockData[k] = *(hexRange.pDataBuffer + i);
                                //Check if this is a signature byte.  If so, replace the value in the buffer
                                //with the post-signing expected signature value, since this is now the expected
                                //value from the device, rather than the value from the hex file...
                                if((address+i) == extendedBootInfo.PIC18.signatureAddress)
                                {
                                    hexEraseBlockData[k] = (unsigned char)extendedBootInfo.PIC18.signatureValue;    //Write LSB of signature into buffer
                                }
                                if((address+i) == (extendedBootInfo.PIC18.signatureAddress + 1))
                                {
                                    hexEraseBlockData[k] = (unsigned char)(extendedBootInfo.PIC18.signatureValue >> 8); //Write MSB into buffer
                                }
                                k++;
                            }
                            if((k >= extendedBootInfo.PIC18.erasePageSize) || (k >= sizeof(hexEraseBlockData)))
                                break;
                        }//for(i = 0; i < rangeSize; i++)
                    }
                }//foreach(hexRange, hexData->ranges)

                //We now have both the hex data and the post signing flash erase block data
                //in two RAM buffers.  Compare them to each other to perform post-signing
                //verify.
                for(i = 0; i < extendedBootInfo.PIC18.erasePageSize; i++)
                {
                    if(flashData[i] != hexEraseBlockData[i])
                    {
                        failureDetected = true;
                        qWarning("Post signing verify failure.");
                        EraseDevice();  //Send an erase command, to forcibly
                        //remove the signature (which might be valid), since
                        //there was a verify error and we can't trust the application
                        //firmware image integrity.  This ensures the device jumps
                        //back into bootloader mode always.

                        errorAddress = startOfEraseBlock + i;
                        expectedResult = hexEraseBlockData[i] + ((uint32_t)hexEraseBlockData[i+1] << 8);
                        //expectedResult = hexEraseBlockData[i];
                        actualResult = flashData[i] + ((uint32_t)flashData[i+1] << 8);
                        //actualResult = flashData[i];

                        break;
                    }
                }//for(i = 0; i < extendedBootInfo.PIC18.erasePageSize; i++)
            }//if(device->family == Device::PIC18)

        }//if(deviceFirmwareIsAtLeast101 == true)

    }//if(failureDetected == false)

    if(failureDetected == true)
    {
        qDebug("Verify failed at address: 0x%x", errorAddress);
        qDebug("Expected result: 0x%x", expectedResult);
        qDebug("Actual result: 0x%x", actualResult);
        emit AppendString("Operation aborted due to error encountered during verify operation.");
        emit AppendString("Please try the erase/program/verify sequence again.");
        emit AppendString("If repeated failures are encountered, this may indicate the flash");
        emit AppendString("memory has worn out, that the device has been damaged, or that");
        emit AppendString("there is some other unidentified problem.");

        emit IoWithDeviceCompleted("Verify", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
    }
    else
    {
        emit IoWithDeviceCompleted("Verify", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        emit AppendString("Erase/Program/Verify sequence completed successfully.");
        emit AppendString("You may now unplug or reset the device.");
    }

    //emit SetProgressBar(100);   //Set progress bar to 100%
}//void MainWindow::VerifyDevice()


//Gets called when the user clicks to program button in the GUI.
void MainWindow::on_actionWrite_Device_triggered()
{
    future = QtConcurrent::run(this, &MainWindow::WriteDevice);
    ui->plainTextEdit->clear();
    ui->plainTextEdit->appendPlainText("Starting Erase/Program/Verify Sequence.");
    ui->plainTextEdit->appendPlainText("Do not unplug device or disconnect power until the operation is fully complete.");
    ui->plainTextEdit->appendPlainText(" ");
}


//This thread programs previously parsed .hex file data into the device's programmable memory regions.
void MainWindow::WriteDevice(void)
{
    QTime elapsed;
    Comm::ErrorCode result;
    DeviceData::MemoryRange hexRange;

    //Update the progress bar so the user knows things are happening.
    //emit SetProgressBar(3);
    //First erase the entire device.
    EraseDevice();

    //Now being re-programming each section based on the info we obtained when
    //we parsed the user's .hex file.

    emit IoWithDeviceStarted("Writing Device...");
    foreach(hexRange, hexData->ranges)
    {
        if(writeFlash && (hexRange.type == PROGRAM_MEMORY))
        {
            //emit IoWithDeviceStarted("Writing Device Program Memory...");
            elapsed.start();

            result = comm->Program(hexRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressFLASH,
                                   device->bytesPerWordFLASH,
                                   device->family,
                                   hexRange.end,
                                   hexRange.pDataBuffer);
        }
        else if(writeEeprom && (hexRange.type ==  EEPROM_MEMORY))
        {
                //emit IoWithDeviceStarted("Writing Device EEPROM Memory...");
                elapsed.start();

                result = comm->Program(hexRange.start,
                                       device->bytesPerPacket,
                                       device->bytesPerAddressEEPROM,
                                       device->bytesPerWordEEPROM,
                                       device->family,
                                       hexRange.end,
                                       hexRange.pDataBuffer);
        }
        else if(writeConfig && (hexRange.type == CONFIG_MEMORY))
        {
            //emit IoWithDeviceStarted("Writing Device Config Words...");
            elapsed.start();

            result = comm->Program(hexRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressConfig,
                                   device->bytesPerWordConfig,
                                   device->family,
                                   hexRange.end,
                                   hexRange.pDataBuffer);
        }
        else
        {
            continue;
        }

        //emit IoWithDeviceCompleted("Writing", result, ((double)elapsed.elapsed()) / 1000);

        if(result != Comm::Success)
        {
            qWarning("Programming failed");
            return;
        }
    }

    emit IoWithDeviceCompleted("Write", result, ((double)elapsed.elapsed()) / 1000);

    VerifyDevice();
}

void MainWindow::on_actionBlank_Check_triggered()
{
    future = QtConcurrent::run(this, &MainWindow::BlankCheckDevice);
}

void MainWindow::BlankCheckDevice(void)
{
    QTime elapsed;
    Comm::ErrorCode result;
    DeviceData::MemoryRange deviceRange;

    elapsed.start();

    foreach(deviceRange, deviceData->ranges)
    {
        if(writeFlash && (deviceRange.type == PROGRAM_MEMORY))
        {
            emit IoWithDeviceStarted("Blank Checking Device's Program Memory...");

            result = comm->GetData(deviceRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressFLASH,
                                   device->bytesPerWordFLASH,
                                   deviceRange.end,
                                   deviceRange.pDataBuffer);


            if(result != Comm::Success)
            {
                qWarning("Blank Check failed");
                emit IoWithDeviceCompleted("Blank Checking Program Memory", result, ((double)elapsed.elapsed()) / 1000);
                return;
            }

            for(unsigned int i = 0; i < ((deviceRange.end - deviceRange.start) * device->bytesPerAddressFLASH); i++)
            {
                if((deviceRange.pDataBuffer[i] != 0xFF) && !((device->family == Device::PIC24) && ((i % 4) == 3)))
                {
                    qWarning("Failed blank check at address 0x%x", deviceRange.start + i);
                    qWarning("The value was 0x%x", deviceRange.pDataBuffer[i]);
                    emit IoWithDeviceCompleted("Blank Check", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
                    return;
                }
            }
            emit IoWithDeviceCompleted("Blank Checking Program Memory", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        }
        else if(writeEeprom && deviceRange.type == EEPROM_MEMORY)
        {
            emit IoWithDeviceStarted("Blank Checking Device's EEPROM Memory...");

            result = comm->GetData(deviceRange.start,
                                   device->bytesPerPacket,
                                   device->bytesPerAddressEEPROM,
                                   device->bytesPerWordEEPROM,
                                   deviceRange.end,
                                   deviceRange.pDataBuffer);


            if(result != Comm::Success)
            {
                qWarning("Blank Check failed");
                emit IoWithDeviceCompleted("Blank Checking EEPROM Memory", result, ((double)elapsed.elapsed()) / 1000);
                return;
            }

            for(unsigned int i = 0; i < ((deviceRange.end - deviceRange.start) * device->bytesPerWordEEPROM); i++)
            {
                if(deviceRange.pDataBuffer[i] != 0xFF)
                {
                    qWarning("Failed blank check at address 0x%x + 0x%x", deviceRange.start, i);
                    qWarning("The value was 0x%x", deviceRange.pDataBuffer[i]);
                    emit IoWithDeviceCompleted("Blank Check", Comm::Fail, ((double)elapsed.elapsed()) / 1000);
                    return;
                }
            }
            emit IoWithDeviceCompleted("Blank Checking EEPROM Memory", Comm::Success, ((double)elapsed.elapsed()) / 1000);
        }
        else
        {
            continue;
        }
    }
}

void MainWindow::on_actionErase_Device_triggered()
{
    future = QtConcurrent::run(this, &MainWindow::EraseDevice);
}

void MainWindow::EraseDevice(void)
{
    QTime elapsed;
    Comm::ErrorCode result;
    Comm::BootInfo bootInfo;


    //if(writeFlash || writeEeprom)
    {
        emit IoWithDeviceStarted("Erasing Device... (no status update until complete, may take several seconds)");
        elapsed.start();

        result = comm->Erase();
        if(result != Comm::Success)
        {
            emit IoWithDeviceCompleted("Erase", result, ((double)elapsed.elapsed()) / 1000);
            return;
        }

        result = comm->ReadBootloaderInfo(&bootInfo);

        emit IoWithDeviceCompleted("Erase", result, ((double)elapsed.elapsed()) / 1000);
    }
}

//Executes when the user clicks the open hex file button on the main form.
void MainWindow::on_actionOpen_triggered()
{
    QString msg, newFileName;
    QTextStream stream(&msg);

    //Create an open file dialog box, so the user can select a .hex file.
    newFileName =
        QFileDialog::getOpenFileName(this, "Open Hex File", fileName, "Hex Files (*.hex *.ehx)");

    if(newFileName.isEmpty())
    {
        return;
    }

    LoadFile(newFileName);
}

void MainWindow::LoadFile(QString newFileName)
{
    QString msg;
    QTextStream stream(&msg);
    QFileInfo nfi(newFileName);

    QApplication::setOverrideCursor(Qt::BusyCursor);

    HexImporter import;
    HexImporter::ErrorCode result;
    Comm::ErrorCode commResultCode;

    hexData->ranges.clear();

    //Print some debug info to the debug window.
    //qDebug(QString("Total Programmable Regions Reported by Device: " + QString::number(deviceData->ranges.count(), 10)).toLatin1());

    //First duplicate the deviceData programmable region list and
    //allocate some RAM buffers to hold the hex data that we are about to import.
    foreach(DeviceData::MemoryRange range, deviceData->ranges)
    {
        //Allocate some RAM for the hex file data we are about to import.
        //Initialize all bytes of the buffer to 0xFF, the default unprogrammed memory value,
        //which is also the "assumed" value, if a value is missing inside the .hex file, but
        //is still included in a programmable memory region.
        range.pDataBuffer = new unsigned char[range.dataBufferLength];
        memset(range.pDataBuffer, 0xFF, range.dataBufferLength);
        hexData->ranges.append(range);

        //Print info regarding the programmable memory region to the debug window.
        //qDebug(QString("Device Programmable Region: [" + QString::number(range.start, 16).toUpper() + " - " +
                   //QString::number(range.end, 16).toUpper() +")").toLatin1());
    }

    //Import the hex file data into the hexData->ranges[].pDataBuffer buffers.
    result = import.ImportHexFile(newFileName, hexData, device);
    //Based on the result of the hex file import operation, decide how to proceed.
    switch(result)
    {
        case HexImporter::Success:
            break;

        case HexImporter::CouldNotOpenFile:
            QApplication::restoreOverrideCursor();
            stream << "Error: Could not open file " << nfi.fileName() << "\n";
            ui->plainTextEdit->appendPlainText(msg);
            return;

        case HexImporter::NoneInRange:
            QApplication::restoreOverrideCursor();
            stream << "No address within range in file: " << nfi.fileName() << ".  Verify the correct firmware image was specified and is designed for your device.\n";
            ui->plainTextEdit->appendPlainText(msg);
            return;

        case HexImporter::ErrorInHexFile:
            QApplication::restoreOverrideCursor();
            stream << "Error in hex file.  Please make sure the firmware image supplied was designed for the device to be programmed. \n";
            ui->plainTextEdit->appendPlainText(msg);
            return;
        case HexImporter::InsufficientMemory:
            QApplication::restoreOverrideCursor();
            stream << "Memory allocation failed.  Please close other applications to free up system RAM and try again. \n";
            ui->plainTextEdit->appendPlainText(msg);
            return;

        default:
            QApplication::restoreOverrideCursor();
            stream << "Failed to import: " << result << "\n";
            ui->plainTextEdit->appendPlainText(msg);
            return;
    }

    //Check if the user has imported a .hex file that doesn't contain config bits in it,
    //even though the user is planning on re-programming the config bits section.
    if(writeConfig && (import.hasConfigBits == false) && device->hasConfig())
    {
        //The user had config bit reprogramming selected, but the hex file opened didn't have config bit
        //data in it.  We should automatically prevent config bit programming, to avoid leaving the device
        //in a broken state following the programming cycle.
        commResultCode = comm->LockUnlockConfig(true); //Lock the config bits.
        if(commResultCode != Comm::Success)
        {
            ui->plainTextEdit->appendPlainText("Unexpected internal error encountered.  Recommend restarting the application to avoid ""bricking"" the device.\n");
        }

        QMessageBox::warning(this, "Warning!", "This HEX file does not contain config bit information.\n\nAutomatically disabling config bit reprogramming to avoid leaving the device in a state that could prevent further bootloading.", QMessageBox::AcceptRole, QMessageBox::AcceptRole);
        writeConfig = false;
    }

    fileName = newFileName;
    watchFileName = newFileName;

    QSettings settings;
    settings.beginGroup("MainWindow");

    QStringList files = settings.value("recentFileList").toStringList();
    files.removeAll(fileName);
    files.prepend(fileName);
    while(files.size() > MAX_RECENT_FILES)
    {
        files.removeLast();
    }
    settings.setValue("recentFileList", files);
    UpdateRecentFileList();

    stream.setIntegerBase(10);

    msg.clear();
    QFileInfo fi(fileName);
    QString name = fi.fileName();
    stream << "Opened: " << name << "\n";
    ui->plainTextEdit->appendPlainText(msg);
    hexOpen = true;
    setBootloadEnabled(true);
    QApplication::restoreOverrideCursor();

    return;
}

void MainWindow::openRecentFile(void)
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (action)
    {
        LoadFile(action->data().toString());
    }
}

void MainWindow::UpdateRecentFileList(void)
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    QStringList files;

    files = settings.value("recentFileList").toStringList();

    int recentFileCount = qMin(files.size(), MAX_RECENT_FILES);
    QString text;
    int i;

    for(i = 0; i < recentFileCount; i++)
    {
        text = tr("&%1 %2").arg(i + 1).arg(QFileInfo(files[i]).fileName());

        recentFiles[i]->setText(text);
        recentFiles[i]->setData(files[i]);
        recentFiles[i]->setVisible(comm->isConnected());
    }

    for(; i < MAX_RECENT_FILES; i++)
    {
        recentFiles[i]->setVisible(false);
    }
}

void MainWindow::on_action_About_triggered()
{
    QString msg;
    QTextStream stream(&msg);

    stream << "USB HID Bootloader v" << VERSION << "\n";
    stream << "Copyright " << (char)Qt::Key_copyright << " 2011-2013,  Microchip Technology Inc.\n\n";

    stream << "Microchip licenses this software to you solely for use with\n";
    stream << "Microchip products. The software is owned by Microchip and\n";
    stream << "its licensors, and is protected under applicable copyright\n";
    stream << "laws. All rights reserved.\n\n";

    stream << "SOFTWARE IS PROVIDED \"AS IS.\"  MICROCHIP EXPRESSLY\n";
    stream << "DISCLAIMS ANY WARRANTY OF ANY KIND, WHETHER EXPRESS\n";
    stream << "OR IMPLIED, INCLUDING BUT NOT LIMITED TO, THE IMPLIED\n";
    stream << "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A\n";
    stream << "PARTICULAR PURPOSE, OR NON-INFRINGEMENT.  IN NO EVENT\n";
    stream << "SHALL MICROCHIP BE LIABLE FOR ANY INCIDENTAL, SPECIAL,\n";
    stream << "INDIRECT OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR\n";
    stream << "LOST DATA, HARM TO YOUR EQUIPMENT, COST OF\n";
    stream << "PROCUREMENT OF SUBSTITUTE GOODS, TECHNOLOGY OR\n";
    stream << "SERVICES, ANY CLAIMS BY THIRD PARTIES (INCLUDING BUT\n";
    stream << "NOT LIMITED TO ANY DEFENSE THEREOF), ANY CLAIMS FOR\n";
    stream << "INDEMNITY OR CONTRIBUTION, OR OTHER SIMILAR COSTS.\n\n";

    stream << "To the fullest extent allowed by law, Microchip and its\n";
    stream << "licensors liability shall not exceed the amount of fees, if any,\n";
    stream << "that you have paid directly to Microchip to use this software.\n\n";

    stream << "MICROCHIP PROVIDES THIS SOFTWARE CONDITIONALLY UPON\n";
    stream << "YOUR ACCEPTANCE OF THESE TERMS.";

    QMessageBox::about(this, "About", msg);
}

void MainWindow::GetQuery()
{
    QTime totalTime;
    Comm::BootInfo bootInfo;
    DeviceData::MemoryRange range;
    QString connectMsg;
    QTextStream ss(&connectMsg);

    QString eeMsg;
    QTextStream ee(&eeMsg);


    qDebug("Executing GetQuery() command.");

    totalTime.start();

    if(!comm->isConnected())
    {
        qWarning("Query not sent, device not connected");
        return;
    }

    //Send the Query command to the device over USB, and check the result status.
    switch(comm->ReadBootloaderInfo(&bootInfo))
    {
        case Comm::Fail:
        case Comm::IncorrectCommand:
            ui->plainTextEdit->appendPlainText("Unable to communicate with device\n");
            return;
        case Comm::Timeout:
            ss << "Operation timed out";
            break;
        case Comm::Success:
            wasBootloaderMode = true;
            ss << "Device Ready";
            deviceLabel.setText("Connected");
            ReadDeviceRDSaddress();
            break;
        default:
            return;
    }

    ss << " (" << (double)totalTime.elapsed() / 1000 << "s)\n";
    ui->plainTextEdit->appendPlainText(connectMsg);
    deviceData->ranges.clear();

    //Now start parsing the bootInfo packet to learn more about the device.  The bootInfo packet contains
    //contains the query response data from the USB device.  We will save these values into globabl variables
    //so other parts of the application can use the info when deciding how to do things.
    device->family = (Device::Families) bootInfo.deviceFamily;
    device->bytesPerPacket = bootInfo.bytesPerPacket;

    //Set some processor family specific global variables that will be used elsewhere (ex: during program/verify operations).
    switch(device->family)
    {
        case Device::PIC18:
            device->bytesPerWordFLASH = 2;
            device->bytesPerAddressFLASH = 1;
            break;
        case Device::PIC24:
            device->bytesPerWordFLASH = 4;
            device->bytesPerAddressFLASH = 2;
            device->bytesPerWordConfig = 4;
            device->bytesPerAddressConfig = 2;
            break;
        case Device::PIC32:
            device->bytesPerWordFLASH = 4;
            device->bytesPerAddressFLASH = 1;
            break;
        case Device::PIC16:
            device->bytesPerWordFLASH = 2;
            device->bytesPerAddressFLASH = 2;
        default:
            device->bytesPerWordFLASH = 2;
            device->bytesPerAddressFLASH = 1;
            break;
    }

    //Initialize the deviceData buffers and length variables, with the regions that the firmware claims are
    //reprogrammable.  We will need this information later, to decide what part(s) of the .hex file we
    //should look at/try to program into the device.  Data sections in the .hex file that are not included
    //in these regions should be ignored.
    for(int i = 0; i < MAX_DATA_REGIONS; i++)
    {
        if(bootInfo.memoryRegions[i].type == END_OF_TYPES_LIST)
        {
            //Before we quit, check the special versionFlag byte,
            //to see if the bootloader firmware is at least version 1.01.
            //If it is, then it will support the extended query command.
            //If the device is based on v1.00 bootloader firmware, it will have
            //loaded the versionFlag location with 0x00, which was a pad byte.
            if(bootInfo.versionFlag == BOOTLOADER_V1_01_OR_NEWER_FLAG)
            {
                deviceFirmwareIsAtLeast101 = true;
                qDebug("Device bootloader firmware is v1.01 or newer and supports Extended Query.");
                //Now fetch the extended query information packet from the USB firmware.
                comm->ReadExtendedQueryInfo(&extendedBootInfo);
                qDebug("Device bootloader firmware version is: " + extendedBootInfo.PIC18.bootloaderVersion);
            }
            else
            {
                deviceFirmwareIsAtLeast101 = false;
            }
            break;
        }

        //Error check: Check the firmware's reported size to make sure it is sensible.  This ensures
        //we don't try to allocate ourselves a massive amount of RAM (capable of crashing this PC app)
        //if the firmware claimed an improper value.
        if(bootInfo.memoryRegions[i].size > MAXIMUM_PROGRAMMABLE_MEMORY_SEGMENT_SIZE)
        {
            bootInfo.memoryRegions[i].size = MAXIMUM_PROGRAMMABLE_MEMORY_SEGMENT_SIZE;
        }

        //Parse the bootInfo response packet and allocate ourselves some RAM to hold the eventual data to program.
        if(bootInfo.memoryRegions[i].type == PROGRAM_MEMORY)
        {
            range.type = PROGRAM_MEMORY;
            range.dataBufferLength = bootInfo.memoryRegions[i].size * device->bytesPerAddressFLASH;
            range.pDataBuffer = new unsigned char[range.dataBufferLength];
            memset(&range.pDataBuffer[0], 0xFF, range.dataBufferLength);
        }
        else if(bootInfo.memoryRegions[i].type == EEPROM_MEMORY)
        {
            range.type = EEPROM_MEMORY;
            range.dataBufferLength = bootInfo.memoryRegions[i].size * device->bytesPerAddressEEPROM;
            range.pDataBuffer = new unsigned char[range.dataBufferLength];
            memset(&range.pDataBuffer[0], 0xFF, range.dataBufferLength);

            ///ui->plainTextEdit->appendPlainText("EEPROM Details\n");

            //ee << "EEPROM " << bootInfo.memoryRegions[i].size << " Addresses  \n";
            //ee << "EEPROM " << device->bytesPerAddressEEPROM << " Bytes per Address  \n";
            //ui->plainTextEdit->appendPlainText(eeMsg);

        }
        else if(bootInfo.memoryRegions[i].type == CONFIG_MEMORY)
        {
            range.type = CONFIG_MEMORY;
            range.dataBufferLength = bootInfo.memoryRegions[i].size * device->bytesPerAddressConfig;
            range.pDataBuffer = new unsigned char[range.dataBufferLength];
            memset(&range.pDataBuffer[0], 0xFF, range.dataBufferLength);
        }

        //Notes regarding range.start and range.end: The range.start is defined as the starting address inside
        //the USB device that will get programmed.  For example, if the bootloader occupies 0x000-0xFFF flash
        //memory addresses (ex: on a PIC18), then the starting bootloader programmable address would typically
        //be = 0x1000 (ex: range.start = 0x1000).
        //The range.end is defined as the last address that actually gets programmed, plus one, in this programmable
        //region.  For example, for a 64kB PIC18 microcontroller, the last implemented flash memory address
        //is 0xFFFF.  If the last 1024 bytes are reserved by the bootloader (since that last page contains the config
        //bits for instance), then the bootloader firmware may only allow the last address to be programmed to
        //be = 0xFBFF.  In this scenario, the range.end value would be = 0xFBFF + 1 = 0xFC00.
        //When this application uses the range.end value, it should be aware that the actual address limit of
        //range.end does not actually get programmed into the device, but the address just below it does.
        //In this example, the programmed region would end up being 0x1000-0xFBFF (even though range.end = 0xFC00).
        //The proper code to program this would basically be something like this:
        //for(i = range.start; i < range.end; i++)
        //{
        //    //Insert code here that progams one device address.  Note: for PIC18 this will be one byte for flash memory.
        //    //For PIC24 this is actually 2 bytes, since the flash memory is addressed as a 16-bit word array.
        //}
        //In the above example, the for() loop exits just before the actual range.end value itself is programmed.

        range.start = bootInfo.memoryRegions[i].address;
        range.end = bootInfo.memoryRegions[i].address + bootInfo.memoryRegions[i].size;
        //Add the new structure+buffer to the list
        deviceData->ranges.append(range);
    }


    //Make sure user has allowed at least one region to be programmed
    if(!(writeFlash || writeEeprom || writeConfig))
    {
        setBootloadEnabled(false);
        ui->action_Settings->setEnabled(true);
    }
    else
        setBootloadEnabled(true);
}




void MainWindow::on_action_Settings_triggered()
{
    Comm::ErrorCode result;
    Settings* dlg = new Settings(this);

    dlg->enableEepromBox(device->hasEeprom());

    dlg->setWriteFlash(writeFlash);
    dlg->setWriteConfig(writeConfig);
    dlg->setWriteEeprom(writeEeprom);

    if(dlg->exec() == QDialog::Accepted)
    {
        writeFlash = dlg->writeFlash;
        writeEeprom = dlg->writeEeprom;

        if(!writeConfig && dlg->writeConfig)
        {
            ui->plainTextEdit->appendPlainText("Disabling Erase button to prevent accidental erasing of the configuration words without reprogramming them\n");
            writeConfig = true;
            hexOpen = false;
            result = comm->LockUnlockConfig(false);
            if(result == Comm::Success)
            {
                ui->plainTextEdit->appendPlainText("Unlocked Configuration bits successfully\n");
                GetQuery();
            }
        }
        else if(writeConfig && !dlg->writeConfig)
        {
            writeConfig = false;
            hexOpen = false;
            result = comm->LockUnlockConfig(true);
            if(result == Comm::Success)
            {
                ui->plainTextEdit->appendPlainText("Locked Configuration bits successfully\n");
                GetQuery();
            }
        }

        if(!(writeFlash || writeEeprom || writeConfig))
        {
            setBootloadEnabled(false);
            ui->action_Settings->setEnabled(true);
        }
        else
        {
            setBootloadEnabled(true);
        }
    }

    delete dlg;
}

void MainWindow::on_actionReset_Device_triggered()
{


    if(!comm->isConnected())
    {
        failed = -1;
        qWarning("Reset not sent, device not connected");
        return;
    }

    ui->plainTextEdit->appendPlainText("Resetting...");
    comm->Reset();
}

void MainWindow::RecalculateFrequencySpacing ()
{
    QString x;
    bool ok;

    NumTones = ui->NumTones->text().toInt(&ok,10);
    N = ui->N->text().toInt(&ok,10);

    SampleTime = (N * 27)/1000;
    CycleTime = NumTones * SampleTime;
    FreqSpacing = 1350/SampleTime;

    x.sprintf("%d",FreqSpacing); ui->FreqSpacing->setText(x);
    x.sprintf("%d",SampleTime); ui->SampleTime->setText(x);
    x.sprintf("%d",CycleTime); ui->CycleTime->setText(x);


}



void MainWindow::CopyBufferToScreen()
{
    // copy from DataBuffer to screen,  this byte by byte method, ( although painfull ) avoids other problems with big-endian vs little endian systems
    //
    QString x,y,s;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    float f;
    int freq;
    int newADDR;

    int R0H,R0L,R1H,R1L,R2H,R2L,R3H,R3L;
    int f1,f2,f3,f4,f5,f6;
    int fadeOn, fadeOff;
    int PatternOn, PatternOff;

    if (DataBuffer[eemap_serial_mode] == 0x01) { ui->GoertzelDisplay->setChecked(true); }
    if (DataBuffer[eemap_serial_mode] == 0x02) { ui->RDSdataDisplay->setChecked(true); }
    if (DataBuffer[eemap_serial_mode] == 0x03) { ui->RDSgroup6Display->setChecked(true); }
    if (DataBuffer[eemap_serial_mode] == 0x04) { ui->rssiDisplay->setChecked(true); }
    if (DataBuffer[eemap_serial_mode] == 0x05) { ui->bitmapDisplay->setChecked(true); }

    if (DataBuffer[eemap_output_mode] == 0x01) { ui->RadioButtonPWMDisable->setChecked(true); }
    if (DataBuffer[eemap_output_mode] == 0x02) { ui->RadioButtonPWMEnable->setChecked(true);  }
    if (DataBuffer[eemap_output_mode] == 0x03) { ui->RadioButtonRGBEnable->setChecked(true);  }
    if (DataBuffer[eemap_output_mode] == 0x04) { ui->RadioButtonOutputToggle->setChecked(true);  }
    if (DataBuffer[eemap_output_mode] == 0x05) { ui->RadioButtonPWMMagnitude->setChecked(true);  }

    if (DataBuffer[eemap_startup_mode] == 0x01) { ui->radioButtonBlink->setChecked(true);           }
    if (DataBuffer[eemap_startup_mode] == 0x02) { ui->radioButtonBreath->setChecked(true);          }
    if (DataBuffer[eemap_startup_mode] == 0x03) { ui->radioButtonSparkle->setChecked(true);         }
    if (DataBuffer[eemap_startup_mode] == 0x04) { ui->radioButtonTwinkle->setChecked(true);         }
    if (DataBuffer[eemap_startup_mode] == 0x05) { ui->radioButtonSignalStrength->setChecked(true);  }
    if (DataBuffer[eemap_startup_mode] == 0x06) { ui->radioButtonToneEnable->setChecked(true);      }
    if (DataBuffer[eemap_startup_mode] == 0x07) { ui->radioToneDecodeDisabled->setChecked(true);    }
    if (DataBuffer[eemap_startup_mode] == 0x08) { ui->radioButtonBitMapMode->setChecked(true);    }

    if (DataBuffer[eemap_save_station] == 0x01) {  ui->radioButtonRememberStation->setChecked(true);     }
    if (DataBuffer[eemap_save_station] == 0x00) {  ui->radioButtonRememberStation->setChecked(false);    }


    if (DataBuffer[eemap_antenna_type_address] == 0x01)      {  ui->InternalAntenna->setChecked(true);     }
    if (DataBuffer[eemap_antenna_type_address] == 0x00)      {  ui->InternalAntenna->setChecked(false);    }


    newADDR=DataBuffer[eemap_device_serial ]*256+DataBuffer[eemap_device_serial+1 ];
    if (ui->ForceAddressChange->isChecked()==true) {
        x.sprintf("%04X",newADDR ); ui->DeviceAddress->setText(x);
        ADDR=newADDR;
    }
    else {
        x.sprintf("%04X",ADDR ); ui->DeviceAddress->setText(x);
    }

    x.sprintf("%04X", DataBuffer[eemap_group_address1]*256+DataBuffer[eemap_group_address1+1]); ui->GroupAddress1->setText(x);
    x.sprintf("%04X", DataBuffer[eemap_group_address2]*256+DataBuffer[eemap_group_address2+1]); ui->GroupAddress2->setText(x);
    x.sprintf("%04X", DataBuffer[eemap_group_address3]*256+DataBuffer[eemap_group_address3+1]); ui->GroupAddress3->setText(x);
    x.sprintf("%04X", DataBuffer[eemap_group_address4]*256+DataBuffer[eemap_group_address4+1]); ui->GroupAddress4->setText(x);
    x.sprintf("%04X", DataBuffer[eemap_group_address5]*256+DataBuffer[eemap_group_address5+1]); ui->GroupAddress5->setText(x);
    x.sprintf("%04X", DataBuffer[eemap_group_address6]*256+DataBuffer[eemap_group_address6+1]); ui->GroupAddress6->setText(x);

    f1=DataBuffer[eemap_tone1]*256+DataBuffer[eemap_tone1+1];  if (f1==0xffff) { f1=0; }
    f2=DataBuffer[eemap_tone2]*256+DataBuffer[eemap_tone2+1];  if (f2==0xffff) { f2=0; }
    f3=DataBuffer[eemap_tone3]*256+DataBuffer[eemap_tone3+1];  if (f3==0xffff) { f3=0; }
    f4=DataBuffer[eemap_tone4]*256+DataBuffer[eemap_tone4+1];  if (f4==0xffff) { f4=0; }
    f5=DataBuffer[eemap_tone5]*256+DataBuffer[eemap_tone5+1];  if (f5==0xffff) { f5=0; }
    f6=DataBuffer[eemap_tone6]*256+DataBuffer[eemap_tone6+1];  if (f6==0xffff) { f6=0; }

    x.sprintf("%4d", f1); ui->Tone1->setText(x);
    x.sprintf("%4d", f2); ui->Tone2->setText(x);
    x.sprintf("%4d", f3); ui->Tone3->setText(x);
    x.sprintf("%4d", f4); ui->Tone4->setText(x);
    x.sprintf("%4d", f5); ui->Tone5->setText(x);
    x.sprintf("%4d", f6); ui->Tone6->setText(x);

    PatternOn = DataBuffer[eemap_pattern_on ];
    if (PatternOn==0 ) { PatternOn = 1;   }
    if (PatternOn> 1 ) { PatternOn = PatternOn*10;   }
    PatternOff=DataBuffer[eemap_pattern_off];
    if (PatternOff==0 ) { PatternOff = 1;   }
    if (PatternOff>1 ) { PatternOff= PatternOff*10;  }

    x.sprintf("%4d", PatternOn); ui->PatternOn->setText(x);
    x.sprintf("%4d", PatternOff); ui->PatternOff->setText(x);

    fadeOn = DataBuffer[eemap_fade_on];
    if (fadeOn==0 ) { fadeOn=1;  }
    if (fadeOn>1 ) { fadeOn= fadeOn*10;  }
    fadeOff=DataBuffer[eemap_fade_off];
    if (fadeOff==0 ) { fadeOff=1;  }
    if (fadeOff>1) { fadeOff=fadeOff*10; }

    x.sprintf("%4d", fadeOn ); ui->FadeOn->setText(x);
    x.sprintf("%4d", fadeOff); ui->FadeOff->setText(x);

    x.sprintf("%4d", DataBuffer[eemap_threshold]*256+DataBuffer[eemap_threshold+1]); ui->Threshold->setText(x);
    x.sprintf("%4d", DataBuffer[eemap_hysteresis]*256+DataBuffer[eemap_hysteresis+1]); ui->Hysteresis->setText(x);

    NumTones=DataBuffer[eemap_number_of_tones];
    N=DataBuffer[eemap_number_of_samples]*256+DataBuffer[eemap_number_of_samples+1];

    x.sprintf("%1d",NumTones ); ui->NumTones->setText(x);
    x.sprintf("%4d",N ); ui->N->setText(x);

    RecalculateFrequencySpacing ();

    // copy bitmap pixels
    R0H=DataBuffer[bitmap   ]*256+DataBuffer[bitmap +1];
    R0L=DataBuffer[bitmap+2 ]*256+DataBuffer[bitmap +3];

    x.sprintf("%04X", R0H); ui->Row0H->setText(x);
    x.sprintf("%04X", R0L); ui->Row0L->setText(x);




    // I give up... how do you do component arrays in Qt..   brute force wins everytime :)

    (R0H & 0x8000) ? ui->R0_31->setChecked(true) : ui->R0_31->setChecked(false);
    (R0H & 0x4000) ? ui->R0_30->setChecked(true) : ui->R0_30->setChecked(false);
    (R0H & 0x2000) ? ui->R0_29->setChecked(true) : ui->R0_29->setChecked(false);
    (R0H & 0x1000) ? ui->R0_28->setChecked(true) : ui->R0_28->setChecked(false);
    (R0H & 0x0800) ? ui->R0_27->setChecked(true) : ui->R0_27->setChecked(false);
    (R0H & 0x0400) ? ui->R0_26->setChecked(true) : ui->R0_26->setChecked(false);
    (R0H & 0x0200) ? ui->R0_25->setChecked(true) : ui->R0_25->setChecked(false);
    (R0H & 0x0100) ? ui->R0_24->setChecked(true) : ui->R0_24->setChecked(false);

    (R0H & 0x0080) ? ui->R0_23->setChecked(true) : ui->R0_23->setChecked(false);
    (R0H & 0x0040) ? ui->R0_22->setChecked(true) : ui->R0_22->setChecked(false);
    (R0H & 0x0020) ? ui->R0_21->setChecked(true) : ui->R0_21->setChecked(false);
    (R0H & 0x0010) ? ui->R0_20->setChecked(true) : ui->R0_20->setChecked(false);
    (R0H & 0x0008) ? ui->R0_19->setChecked(true) : ui->R0_19->setChecked(false);
    (R0H & 0x0004) ? ui->R0_18->setChecked(true) : ui->R0_18->setChecked(false);
    (R0H & 0x0002) ? ui->R0_17->setChecked(true) : ui->R0_17->setChecked(false);
    (R0H & 0x0001) ? ui->R0_16->setChecked(true) : ui->R0_16->setChecked(false);

    (R0L & 0x8000) ? ui->R0_15->setChecked(true) : ui->R0_15->setChecked(false);
    (R0L & 0x4000) ? ui->R0_14->setChecked(true) : ui->R0_14->setChecked(false);
    (R0L & 0x2000) ? ui->R0_13->setChecked(true) : ui->R0_13->setChecked(false);
    (R0L & 0x1000) ? ui->R0_12->setChecked(true) : ui->R0_12->setChecked(false);
    (R0L & 0x0800) ? ui->R0_11->setChecked(true) : ui->R0_11->setChecked(false);
    (R0L & 0x0400) ? ui->R0_10->setChecked(true) : ui->R0_10->setChecked(false);
    (R0L & 0x0200) ? ui->R0_9->setChecked(true)  : ui->R0_9->setChecked(false);
    (R0L & 0x0100) ? ui->R0_8->setChecked(true)  : ui->R0_8->setChecked(false);

    (R0L & 0x0080) ? ui->R0_7->setChecked(true)  : ui->R0_7->setChecked(false);
    (R0L & 0x0040) ? ui->R0_6->setChecked(true)  : ui->R0_6->setChecked(false);
    (R0L & 0x0020) ? ui->R0_5->setChecked(true)  : ui->R0_5->setChecked(false);
    (R0L & 0x0010) ? ui->R0_4->setChecked(true)  : ui->R0_4->setChecked(false);
    (R0L & 0x0008) ? ui->R0_3->setChecked(true)  : ui->R0_3->setChecked(false);
    (R0L & 0x0004) ? ui->R0_2->setChecked(true)  : ui->R0_2->setChecked(false);
    (R0L & 0x0002) ? ui->R0_1->setChecked(true)  : ui->R0_1->setChecked(false);
    (R0L & 0x0001) ? ui->R0_0->setChecked(true)  : ui->R0_0->setChecked(false);

    R1H=DataBuffer[bitmap+4 ]*256+DataBuffer[bitmap +5];
    R1L=DataBuffer[bitmap+6 ]*256+DataBuffer[bitmap +7];

    x.sprintf("%04X", R1H); ui->Row1H->setText(x);
    x.sprintf("%04X", R1L); ui->Row1L->setText(x);

    (R1H & 0x8000) ? ui->R1_31->setChecked(true) : ui->R1_31->setChecked(false);
    (R1H & 0x4000) ? ui->R1_30->setChecked(true) : ui->R1_30->setChecked(false);
    (R1H & 0x2000) ? ui->R1_29->setChecked(true) : ui->R1_29->setChecked(false);
    (R1H & 0x1000) ? ui->R1_28->setChecked(true) : ui->R1_28->setChecked(false);
    (R1H & 0x0800) ? ui->R1_27->setChecked(true) : ui->R1_27->setChecked(false);
    (R1H & 0x0400) ? ui->R1_26->setChecked(true) : ui->R1_26->setChecked(false);
    (R1H & 0x0200) ? ui->R1_25->setChecked(true) : ui->R1_25->setChecked(false);
    (R1H & 0x0100) ? ui->R1_24->setChecked(true) : ui->R1_24->setChecked(false);

    (R1H & 0x0080) ? ui->R1_23->setChecked(true) : ui->R1_23->setChecked(false);
    (R1H & 0x0040) ? ui->R1_22->setChecked(true) : ui->R1_22->setChecked(false);
    (R1H & 0x0020) ? ui->R1_21->setChecked(true) : ui->R1_21->setChecked(false);
    (R1H & 0x0010) ? ui->R1_20->setChecked(true) : ui->R1_20->setChecked(false);
    (R1H & 0x0008) ? ui->R1_19->setChecked(true) : ui->R1_19->setChecked(false);
    (R1H & 0x0004) ? ui->R1_18->setChecked(true) : ui->R1_18->setChecked(false);
    (R1H & 0x0002) ? ui->R1_17->setChecked(true) : ui->R1_17->setChecked(false);
    (R1H & 0x0001) ? ui->R1_16->setChecked(true) : ui->R1_16->setChecked(false);

    (R1L & 0x8000) ? ui->R1_15->setChecked(true) : ui->R1_15->setChecked(false);
    (R1L & 0x4000) ? ui->R1_14->setChecked(true) : ui->R1_14->setChecked(false);
    (R1L & 0x2000) ? ui->R1_13->setChecked(true) : ui->R1_13->setChecked(false);
    (R1L & 0x1000) ? ui->R1_12->setChecked(true) : ui->R1_12->setChecked(false);
    (R1L & 0x0800) ? ui->R1_11->setChecked(true) : ui->R1_11->setChecked(false);
    (R1L & 0x0400) ? ui->R1_10->setChecked(true) : ui->R1_10->setChecked(false);
    (R1L & 0x0200) ? ui->R1_9->setChecked(true)  : ui->R1_9->setChecked(false);
    (R1L & 0x0100) ? ui->R1_8->setChecked(true)  : ui->R1_8->setChecked(false);

    (R1L & 0x0080) ? ui->R1_7->setChecked(true)  : ui->R1_7->setChecked(false);
    (R1L & 0x0040) ? ui->R1_6->setChecked(true)  : ui->R1_6->setChecked(false);
    (R1L & 0x0020) ? ui->R1_5->setChecked(true)  : ui->R1_5->setChecked(false);
    (R1L & 0x0010) ? ui->R1_4->setChecked(true)  : ui->R1_4->setChecked(false);
    (R1L & 0x0008) ? ui->R1_3->setChecked(true)  : ui->R1_3->setChecked(false);
    (R1L & 0x0004) ? ui->R1_2->setChecked(true)  : ui->R1_2->setChecked(false);
    (R1L & 0x0002) ? ui->R1_1->setChecked(true)  : ui->R1_1->setChecked(false);
    (R1L & 0x0001) ? ui->R1_0->setChecked(true)  : ui->R1_0->setChecked(false);


    R2H=DataBuffer[bitmap+8 ]*256+DataBuffer[bitmap +9];
    R2L=DataBuffer[bitmap+10 ]*256+DataBuffer[bitmap +11];

    x.sprintf("%04X", R2H); ui->Row2H->setText(x);
    x.sprintf("%04X", R2L); ui->Row2L->setText(x);

    (R2H & 0x8000) ? ui->R2_31->setChecked(true) : ui->R2_31->setChecked(false);
    (R2H & 0x4000) ? ui->R2_30->setChecked(true) : ui->R2_30->setChecked(false);
    (R2H & 0x2000) ? ui->R2_29->setChecked(true) : ui->R2_29->setChecked(false);
    (R2H & 0x1000) ? ui->R2_28->setChecked(true) : ui->R2_28->setChecked(false);
    (R2H & 0x0800) ? ui->R2_27->setChecked(true) : ui->R2_27->setChecked(false);
    (R2H & 0x0400) ? ui->R2_26->setChecked(true) : ui->R2_26->setChecked(false);
    (R2H & 0x0200) ? ui->R2_25->setChecked(true) : ui->R2_25->setChecked(false);
    (R2H & 0x0100) ? ui->R2_24->setChecked(true) : ui->R2_24->setChecked(false);

    (R2H & 0x0080) ? ui->R2_23->setChecked(true) : ui->R2_23->setChecked(false);
    (R2H & 0x0040) ? ui->R2_22->setChecked(true) : ui->R2_22->setChecked(false);
    (R2H & 0x0020) ? ui->R2_21->setChecked(true) : ui->R2_21->setChecked(false);
    (R2H & 0x0010) ? ui->R2_20->setChecked(true) : ui->R2_20->setChecked(false);
    (R2H & 0x0008) ? ui->R2_19->setChecked(true) : ui->R2_19->setChecked(false);
    (R2H & 0x0004) ? ui->R2_18->setChecked(true) : ui->R2_18->setChecked(false);
    (R2H & 0x0002) ? ui->R2_17->setChecked(true) : ui->R2_17->setChecked(false);
    (R2H & 0x0001) ? ui->R2_16->setChecked(true) : ui->R2_16->setChecked(false);

    (R2L & 0x8000) ? ui->R2_15->setChecked(true) : ui->R2_15->setChecked(false);
    (R2L & 0x4000) ? ui->R2_14->setChecked(true) : ui->R2_14->setChecked(false);
    (R2L & 0x2000) ? ui->R2_13->setChecked(true) : ui->R2_13->setChecked(false);
    (R2L & 0x1000) ? ui->R2_12->setChecked(true) : ui->R2_12->setChecked(false);
    (R2L & 0x0800) ? ui->R2_11->setChecked(true) : ui->R2_11->setChecked(false);
    (R2L & 0x0400) ? ui->R2_10->setChecked(true) : ui->R2_10->setChecked(false);
    (R2L & 0x0200) ? ui->R2_9->setChecked(true)  : ui->R2_9->setChecked(false);
    (R2L & 0x0100) ? ui->R2_8->setChecked(true)  : ui->R2_8->setChecked(false);

    (R2L & 0x0080) ? ui->R2_7->setChecked(true)  : ui->R2_7->setChecked(false);
    (R2L & 0x0040) ? ui->R2_6->setChecked(true)  : ui->R2_6->setChecked(false);
    (R2L & 0x0020) ? ui->R2_5->setChecked(true)  : ui->R2_5->setChecked(false);
    (R2L & 0x0010) ? ui->R2_4->setChecked(true)  : ui->R2_4->setChecked(false);
    (R2L & 0x0008) ? ui->R2_3->setChecked(true)  : ui->R2_3->setChecked(false);
    (R2L & 0x0004) ? ui->R2_2->setChecked(true)  : ui->R2_2->setChecked(false);
    (R2L & 0x0002) ? ui->R2_1->setChecked(true)  : ui->R2_1->setChecked(false);
    (R2L & 0x0001) ? ui->R2_0->setChecked(true)  : ui->R2_0->setChecked(false);


    R3H=DataBuffer[bitmap+12 ]*256+DataBuffer[bitmap +13];
    R3L=DataBuffer[bitmap+14 ]*256+DataBuffer[bitmap +15];

    x.sprintf("%04X", R3H); ui->Row3H->setText(x);
    x.sprintf("%04X", R3L); ui->Row3L->setText(x);

    (R3H & 0x8000) ? ui->R3_31->setChecked(true) : ui->R3_31->setChecked(false);
    (R3H & 0x4000) ? ui->R3_30->setChecked(true) : ui->R3_30->setChecked(false);
    (R3H & 0x2000) ? ui->R3_29->setChecked(true) : ui->R3_29->setChecked(false);
    (R3H & 0x1000) ? ui->R3_28->setChecked(true) : ui->R3_28->setChecked(false);
    (R3H & 0x0800) ? ui->R3_27->setChecked(true) : ui->R3_27->setChecked(false);
    (R3H & 0x0400) ? ui->R3_26->setChecked(true) : ui->R3_26->setChecked(false);
    (R3H & 0x0200) ? ui->R3_25->setChecked(true) : ui->R3_25->setChecked(false);
    (R3H & 0x0100) ? ui->R3_24->setChecked(true) : ui->R3_24->setChecked(false);

    (R3H & 0x0080) ? ui->R3_23->setChecked(true) : ui->R3_23->setChecked(false);
    (R3H & 0x0040) ? ui->R3_22->setChecked(true) : ui->R3_22->setChecked(false);
    (R3H & 0x0020) ? ui->R3_21->setChecked(true) : ui->R3_21->setChecked(false);
    (R3H & 0x0010) ? ui->R3_20->setChecked(true) : ui->R3_20->setChecked(false);
    (R3H & 0x0008) ? ui->R3_19->setChecked(true) : ui->R3_19->setChecked(false);
    (R3H & 0x0004) ? ui->R3_18->setChecked(true) : ui->R3_18->setChecked(false);
    (R3H & 0x0002) ? ui->R3_17->setChecked(true) : ui->R3_17->setChecked(false);
    (R3H & 0x0001) ? ui->R3_16->setChecked(true) : ui->R3_16->setChecked(false);

    (R3L & 0x8000) ? ui->R3_15->setChecked(true) : ui->R3_15->setChecked(false);
    (R3L & 0x4000) ? ui->R3_14->setChecked(true) : ui->R3_14->setChecked(false);
    (R3L & 0x2000) ? ui->R3_13->setChecked(true) : ui->R3_13->setChecked(false);
    (R3L & 0x1000) ? ui->R3_12->setChecked(true) : ui->R3_12->setChecked(false);
    (R3L & 0x0800) ? ui->R3_11->setChecked(true) : ui->R3_11->setChecked(false);
    (R3L & 0x0400) ? ui->R3_10->setChecked(true) : ui->R3_10->setChecked(false);
    (R3L & 0x0200) ? ui->R3_9->setChecked(true)  : ui->R3_9->setChecked(false);
    (R3L & 0x0100) ? ui->R3_8->setChecked(true)  : ui->R3_8->setChecked(false);

    (R3L & 0x0080) ? ui->R3_7->setChecked(true)  : ui->R3_7->setChecked(false);
    (R3L & 0x0040) ? ui->R3_6->setChecked(true)  : ui->R3_6->setChecked(false);
    (R3L & 0x0020) ? ui->R3_5->setChecked(true)  : ui->R3_5->setChecked(false);
    (R3L & 0x0010) ? ui->R3_4->setChecked(true)  : ui->R3_4->setChecked(false);
    (R3L & 0x0008) ? ui->R3_3->setChecked(true)  : ui->R3_3->setChecked(false);
    (R3L & 0x0004) ? ui->R3_2->setChecked(true)  : ui->R3_2->setChecked(false);
    (R3L & 0x0002) ? ui->R3_1->setChecked(true)  : ui->R3_1->setChecked(false);
    (R3L & 0x0001) ? ui->R3_0->setChecked(true)  : ui->R3_0->setChecked(false);


    freq=DataBuffer[eemap_radio_frequency]*256+DataBuffer[eemap_radio_frequency+1];
    f=((float)(freq)+0.005)/100;
    ui->RadioFrequency->setValue(f);

    if (DataBuffer[eemap_antenna_type_address]== 0x00)
    {
        ui->ExternalAntenna->setChecked(true);
        ui->InternalAntenna->setChecked(false);
    }
    else
    {
        ui->InternalAntenna->setChecked(true);
        ui->ExternalAntenna->setChecked(false);
    }

    x.sprintf("%02d", DataBuffer[eemap_firmware_version_major]);
    y.sprintf("%02d", DataBuffer[eemap_firmware_version_minor]);
    s = x + "." + y;
    ui->FirmwareVersion->setText(s);

    if (DataBuffer[eemap_firmware_version_major]==0xff)
    {
        ui->FirmwareVersion->setText("KW2012");
        ui->Tone4->setEnabled(false);
        ui->Tone5->setEnabled(false);
        ui->Tone6->setEnabled(false);
        ui->Threshold->setEnabled(false);
        ui->Hysteresis->setEnabled(false);
        ui->StartModeFrame->setEnabled(false);
        ui->OutputModeFrame->setEnabled(false);
        ui->RadioFrequency->setEnabled(false);
        ui->InternalAntenna->setEnabled(false);
        ui->ExternalAntenna->setEnabled(false);
        ui->bitmap_frame->setEnabled(false);
        x.sprintf("%4d",500 ); ui->N->setText(x);
        UpdateNumberOfTones();

    }
    else {

        ui->Tone4->setEnabled(true);
        ui->Tone5->setEnabled(true);
        ui->Tone6->setEnabled(true);
        ui->Threshold->setEnabled(true);
        ui->Hysteresis->setEnabled(true);
        ui->StartModeFrame->setEnabled(true);
        ui->OutputModeFrame->setEnabled(true);
        ui->RadioFrequency->setEnabled(true);
        ui->InternalAntenna->setEnabled(true);
        ui->ExternalAntenna->setEnabled(true);
        ui->bitmap_frame->setEnabled(true);
    }
}

void MainWindow::ScreenToBuffer()
{
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int n;
    bool ok;
    float f;
    int R0L, R0H, R1L, R1H, R2L, R2H, R3L, R3H;

    if (ui->radioButtonBlink->isChecked()==true)          { DataBuffer[eemap_startup_mode] = 0x01;  }
    if (ui->radioButtonBreath->isChecked()==true)         { DataBuffer[eemap_startup_mode] = 0x02;  }
    if (ui->radioButtonSparkle->isChecked()==true)        { DataBuffer[eemap_startup_mode] = 0x03;  }
    if (ui->radioButtonTwinkle->isChecked()==true)        { DataBuffer[eemap_startup_mode] = 0x04;  }
    if (ui->radioButtonSignalStrength->isChecked()==true) { DataBuffer[eemap_startup_mode] = 0x05;  }
    if (ui->radioButtonToneEnable->isChecked()==true)     { DataBuffer[eemap_startup_mode] = 0x06;  }
    if (ui->radioToneDecodeDisabled->isChecked()==true)   { DataBuffer[eemap_startup_mode] = 0x07;  }
    if (ui->radioButtonBitMapMode->isChecked()==true)     { DataBuffer[eemap_startup_mode] = 0x08;  }

    if (ui->GoertzelDisplay->isChecked()==true)  { DataBuffer[eemap_serial_mode] = 0x01; }
    if (ui->RDSdataDisplay->isChecked()==true)   { DataBuffer[eemap_serial_mode] = 0x02; }
    if (ui->RDSgroup6Display->isChecked()==true) { DataBuffer[eemap_serial_mode] = 0x03; }
    if (ui->rssiDisplay->isChecked()==true)      { DataBuffer[eemap_serial_mode] = 0x04; }
    if (ui->bitmapDisplay->isChecked()==true)    { DataBuffer[eemap_serial_mode] = 0x05; }

    if (ui->RadioButtonPWMDisable->isChecked()==true)     { DataBuffer[eemap_output_mode] = 0x01; }
    if (ui->RadioButtonPWMEnable->isChecked()==true)      { DataBuffer[eemap_output_mode] = 0x02; }
    if (ui->RadioButtonRGBEnable->isChecked()==true)      { DataBuffer[eemap_output_mode] = 0x03; }
    if (ui->RadioButtonOutputToggle->isChecked()==true)   { DataBuffer[eemap_output_mode] = 0x04; }
    if (ui->RadioButtonPWMMagnitude->isChecked()==true)   { DataBuffer[eemap_output_mode] = 0x05; }

    n = ui->DeviceAddress->text().toInt(&ok,16);  DataBuffer[eemap_device_serial ]=(n>>8)&0xff; DataBuffer[eemap_device_serial+1 ]=n&0xff;
    n = ui->GroupAddress1->text().toInt(&ok,16);  DataBuffer[eemap_group_address1]=(n>>8)&0xff; DataBuffer[eemap_group_address1+1]=n&0xff;
    n = ui->GroupAddress2->text().toInt(&ok,16);  DataBuffer[eemap_group_address2]=(n>>8)&0xff; DataBuffer[eemap_group_address2+1]=n&0xff;
    n = ui->GroupAddress3->text().toInt(&ok,16);  DataBuffer[eemap_group_address3]=(n>>8)&0xff; DataBuffer[eemap_group_address3+1]=n&0xff;
    n = ui->GroupAddress4->text().toInt(&ok,16);  DataBuffer[eemap_group_address4]=(n>>8)&0xff; DataBuffer[eemap_group_address4+1]=n&0xff;
    n = ui->GroupAddress5->text().toInt(&ok,16);  DataBuffer[eemap_group_address5]=(n>>8)&0xff; DataBuffer[eemap_group_address5+1]=n&0xff;
    n = ui->GroupAddress6->text().toInt(&ok,16);  DataBuffer[eemap_group_address6]=(n>>8)&0xff; DataBuffer[eemap_group_address6+1]=n&0xff;

    n = ui->Tone1->text().toInt(&ok,10); DataBuffer[eemap_tone1]=(n>>8)&0xff; DataBuffer[eemap_tone1+1]=n&0xff;
    n = ui->Tone2->text().toInt(&ok,10); DataBuffer[eemap_tone2]=(n>>8)&0xff; DataBuffer[eemap_tone2+1]=n&0xff;
    n = ui->Tone3->text().toInt(&ok,10); DataBuffer[eemap_tone3]=(n>>8)&0xff; DataBuffer[eemap_tone3+1]=n&0xff;
    n = ui->Tone4->text().toInt(&ok,10); DataBuffer[eemap_tone4]=(n>>8)&0xff; DataBuffer[eemap_tone4+1]=n&0xff;
    n = ui->Tone5->text().toInt(&ok,10); DataBuffer[eemap_tone5]=(n>>8)&0xff; DataBuffer[eemap_tone5+1]=n&0xff;
    n = ui->Tone6->text().toInt(&ok,10); DataBuffer[eemap_tone6]=(n>>8)&0xff; DataBuffer[eemap_tone6+1]=n&0xff;

    n = ui->PatternOn->text().toInt(&ok,10);
    if (n==0)  { n=1; }
    if (n>10)  { n=n/10; }
    if (n>255) { n=255; }
    DataBuffer[eemap_pattern_on ]=n;

    n = ui->PatternOff->text().toInt(&ok,10);
    if (n==0)  { n=1; }
    if (n>10)  { n=n/10; }
    if (n>255) { n=255; }
    DataBuffer[eemap_pattern_off]=n;

    n = ui->FadeOn->text().toInt(&ok,10);
    if (n==0)  { n=1; }
    if (n>10)  { n=n/10; }
    if (n>255) { n=255; }
    DataBuffer[eemap_fade_on]=n;

    n = ui->FadeOff->text().toInt(&ok,10);
    if (n==0)  { n=1; }
    if (n>10)  { n=n/10; }
    if (n>255) { n=255; }
    DataBuffer[eemap_fade_off]=n;

    n = ui->Threshold->text().toInt(&ok,10);  DataBuffer[eemap_threshold]=(n>>8)&0xff;  DataBuffer[eemap_threshold+1 ]=n&0xff;
    n = ui->Hysteresis->text().toInt(&ok,10); DataBuffer[eemap_hysteresis]=(n>>8)&0xff; DataBuffer[eemap_hysteresis+1]=n&0xff;

    n = ui->NumTones->text().toInt(&ok,10);    DataBuffer[eemap_number_of_tones]=n&0xff;
    n = ui->N->text().toInt(&ok,10); DataBuffer[eemap_number_of_samples]=(n>>8)&0xff; DataBuffer[eemap_number_of_samples+1]=n&0xff;

    x = ui->RadioFrequency->cleanText();

    x.replace(",", ".");

    f = x.toDouble(&ok);
    f = f + 0.005;
    n = (int)(f*100); DataBuffer[eemap_radio_frequency]=(n>>8)&0xff; DataBuffer[eemap_radio_frequency+1]=n&0xff;

    if (ui->ExternalAntenna->isChecked()==true)      { DataBuffer[eemap_antenna_type_address] = 0x00; }
    if (ui->InternalAntenna->isChecked()==true)      { DataBuffer[eemap_antenna_type_address] = 0x01; }

    if (ui->radioButtonRememberStation->isChecked()==true ) { DataBuffer[eemap_save_station] = 0x01; }
    if (ui->radioButtonRememberStation->isChecked()==false) { DataBuffer[eemap_save_station] = 0x00; }

    R0L=0; R0H=0;
    if (ui->R0_0->isChecked())  R0L|= 0x0001;
    if (ui->R0_1->isChecked())  R0L|= 0x0002;
    if (ui->R0_2->isChecked())  R0L|= 0x0004;
    if (ui->R0_3->isChecked())  R0L|= 0x0008;

    if (ui->R0_4->isChecked())  R0L|= 0x0010;
    if (ui->R0_5->isChecked())  R0L|= 0x0020;
    if (ui->R0_6->isChecked())  R0L|= 0x0040;
    if (ui->R0_7->isChecked())  R0L|= 0x0080;

    if (ui->R0_8->isChecked())  R0L|= 0x0100;
    if (ui->R0_9->isChecked())  R0L|= 0x0200;
    if (ui->R0_10->isChecked()) R0L|= 0x0400;
    if (ui->R0_11->isChecked()) R0L|= 0x0800;

    if (ui->R0_12->isChecked()) R0L|= 0x1000;
    if (ui->R0_13->isChecked()) R0L|= 0x2000;
    if (ui->R0_14->isChecked()) R0L|= 0x4000;
    if (ui->R0_15->isChecked()) R0L|= 0x8000;

    if (ui->R0_16->isChecked())  R0H|= 0x0001;
    if (ui->R0_17->isChecked())  R0H|= 0x0002;
    if (ui->R0_18->isChecked())  R0H|= 0x0004;
    if (ui->R0_19->isChecked())  R0H|= 0x0008;

    if (ui->R0_20->isChecked())  R0H|= 0x0010;
    if (ui->R0_21->isChecked())  R0H|= 0x0020;
    if (ui->R0_22->isChecked())  R0H|= 0x0040;
    if (ui->R0_23->isChecked())  R0H|= 0x0080;

    if (ui->R0_24->isChecked())  R0H|= 0x0100;
    if (ui->R0_25->isChecked())  R0H|= 0x0200;
    if (ui->R0_26->isChecked())  R0H|= 0x0400;
    if (ui->R0_27->isChecked())  R0H|= 0x0800;

    if (ui->R0_28->isChecked())  R0H|= 0x1000;
    if (ui->R0_29->isChecked())  R0H|= 0x2000;
    if (ui->R0_30->isChecked())  R0H|= 0x4000;
    if (ui->R0_31->isChecked())  R0H|= 0x8000;

    x.sprintf("%04X", R0H); ui->Row0H->setText(x);
    x.sprintf("%04X", R0L); ui->Row0L->setText(x);

    DataBuffer[bitmap +0] =(R0H>>8)&0xff;    DataBuffer[bitmap+1]=R0H&0xff;
    DataBuffer[bitmap +2] =(R0L>>8)&0xff;    DataBuffer[bitmap+3]=R0L&0xff;

    R1L=0; R1H=0;
    if (ui->R1_0->isChecked())  R1L|= 0x0001;
    if (ui->R1_1->isChecked())  R1L|= 0x0002;
    if (ui->R1_2->isChecked())  R1L|= 0x0004;
    if (ui->R1_3->isChecked())  R1L|= 0x0008;

    if (ui->R1_4->isChecked())  R1L|= 0x0010;
    if (ui->R1_5->isChecked())  R1L|= 0x0020;
    if (ui->R1_6->isChecked())  R1L|= 0x0040;
    if (ui->R1_7->isChecked())  R1L|= 0x0080;

    if (ui->R1_8->isChecked())  R1L|= 0x0100;
    if (ui->R1_9->isChecked())  R1L|= 0x0200;
    if (ui->R1_10->isChecked()) R1L|= 0x0400;
    if (ui->R1_11->isChecked()) R1L|= 0x0800;

    if (ui->R1_12->isChecked()) R1L|= 0x1000;
    if (ui->R1_13->isChecked()) R1L|= 0x2000;
    if (ui->R1_14->isChecked()) R1L|= 0x4000;
    if (ui->R1_15->isChecked()) R1L|= 0x8000;

    if (ui->R1_16->isChecked())  R1H|= 0x0001;
    if (ui->R1_17->isChecked())  R1H|= 0x0002;
    if (ui->R1_18->isChecked())  R1H|= 0x0004;
    if (ui->R1_19->isChecked())  R1H|= 0x0008;

    if (ui->R1_20->isChecked())  R1H|= 0x0010;
    if (ui->R1_21->isChecked())  R1H|= 0x0020;
    if (ui->R1_22->isChecked())  R1H|= 0x0040;
    if (ui->R1_23->isChecked())  R1H|= 0x0080;

    if (ui->R1_24->isChecked())  R1H|= 0x0100;
    if (ui->R1_25->isChecked())  R1H|= 0x0200;
    if (ui->R1_26->isChecked())  R1H|= 0x0400;
    if (ui->R1_27->isChecked())  R1H|= 0x0800;

    if (ui->R1_28->isChecked())  R1H|= 0x1000;
    if (ui->R1_29->isChecked())  R1H|= 0x2000;
    if (ui->R1_30->isChecked())  R1H|= 0x4000;
    if (ui->R1_31->isChecked())  R1H|= 0x8000;

    x.sprintf("%04X", R1H); ui->Row1H->setText(x);
    x.sprintf("%04X", R1L); ui->Row1L->setText(x);

    DataBuffer[bitmap +4] =(R1H>>8)&0xff;    DataBuffer[bitmap+5]=R1H&0xff;
    DataBuffer[bitmap +6] =(R1L>>8)&0xff;    DataBuffer[bitmap+7]=R1L&0xff;

    R2L=0; R2H=0;
    if (ui->R2_0->isChecked())  R2L|= 0x0001;
    if (ui->R2_1->isChecked())  R2L|= 0x0002;
    if (ui->R2_2->isChecked())  R2L|= 0x0004;
    if (ui->R2_3->isChecked())  R2L|= 0x0008;

    if (ui->R2_4->isChecked())  R2L|= 0x0010;
    if (ui->R2_5->isChecked())  R2L|= 0x0020;
    if (ui->R2_6->isChecked())  R2L|= 0x0040;
    if (ui->R2_7->isChecked())  R2L|= 0x0080;

    if (ui->R2_8->isChecked())  R2L|= 0x0100;
    if (ui->R2_9->isChecked())  R2L|= 0x0200;
    if (ui->R2_10->isChecked()) R2L|= 0x0400;
    if (ui->R2_11->isChecked()) R2L|= 0x0800;

    if (ui->R2_12->isChecked()) R2L|= 0x1000;
    if (ui->R2_13->isChecked()) R2L|= 0x2000;
    if (ui->R2_14->isChecked()) R2L|= 0x4000;
    if (ui->R2_15->isChecked()) R2L|= 0x8000;

    if (ui->R2_16->isChecked())  R2H|= 0x0001;
    if (ui->R2_17->isChecked())  R2H|= 0x0002;
    if (ui->R2_18->isChecked())  R2H|= 0x0004;
    if (ui->R2_19->isChecked())  R2H|= 0x0008;

    if (ui->R2_20->isChecked())  R2H|= 0x0010;
    if (ui->R2_21->isChecked())  R2H|= 0x0020;
    if (ui->R2_22->isChecked())  R2H|= 0x0040;
    if (ui->R2_23->isChecked())  R2H|= 0x0080;

    if (ui->R2_24->isChecked())  R2H|= 0x0100;
    if (ui->R2_25->isChecked())  R2H|= 0x0200;
    if (ui->R2_26->isChecked())  R2H|= 0x0400;
    if (ui->R2_27->isChecked())  R2H|= 0x0800;

    if (ui->R2_28->isChecked())  R2H|= 0x1000;
    if (ui->R2_29->isChecked())  R2H|= 0x2000;
    if (ui->R2_30->isChecked())  R2H|= 0x4000;
    if (ui->R2_31->isChecked())  R2H|= 0x8000;

    x.sprintf("%04X", R2H); ui->Row2H->setText(x);
    x.sprintf("%04X", R2L); ui->Row2L->setText(x);

    DataBuffer[bitmap +8]  =(R2H>>8)&0xff;    DataBuffer[bitmap +9]=R2H&0xff;
    DataBuffer[bitmap +10] =(R2L>>8)&0xff;    DataBuffer[bitmap+11]=R2L&0xff;

    R3L=0; R3H=0;
    if (ui->R3_0->isChecked())  R3L|= 0x0001;
    if (ui->R3_1->isChecked())  R3L|= 0x0002;
    if (ui->R3_2->isChecked())  R3L|= 0x0004;
    if (ui->R3_3->isChecked())  R3L|= 0x0008;

    if (ui->R3_4->isChecked())  R3L|= 0x0010;
    if (ui->R3_5->isChecked())  R3L|= 0x0020;
    if (ui->R3_6->isChecked())  R3L|= 0x0040;
    if (ui->R3_7->isChecked())  R3L|= 0x0080;

    if (ui->R3_8->isChecked())  R3L|= 0x0100;
    if (ui->R3_9->isChecked())  R3L|= 0x0200;
    if (ui->R3_10->isChecked()) R3L|= 0x0400;
    if (ui->R3_11->isChecked()) R3L|= 0x0800;

    if (ui->R3_12->isChecked()) R3L|= 0x1000;
    if (ui->R3_13->isChecked()) R3L|= 0x2000;
    if (ui->R3_14->isChecked()) R3L|= 0x4000;
    if (ui->R3_15->isChecked()) R3L|= 0x8000;

    if (ui->R3_16->isChecked())  R3H|= 0x0001;
    if (ui->R3_17->isChecked())  R3H|= 0x0002;
    if (ui->R3_18->isChecked())  R3H|= 0x0004;
    if (ui->R3_19->isChecked())  R3H|= 0x0008;

    if (ui->R3_20->isChecked())  R3H|= 0x0010;
    if (ui->R3_21->isChecked())  R3H|= 0x0020;
    if (ui->R3_22->isChecked())  R3H|= 0x0040;
    if (ui->R3_23->isChecked())  R3H|= 0x0080;

    if (ui->R3_24->isChecked())  R3H|= 0x0100;
    if (ui->R3_25->isChecked())  R3H|= 0x0200;
    if (ui->R3_26->isChecked())  R3H|= 0x0400;
    if (ui->R3_27->isChecked())  R3H|= 0x0800;

    if (ui->R3_28->isChecked())  R3H|= 0x1000;
    if (ui->R3_29->isChecked())  R3H|= 0x2000;
    if (ui->R3_30->isChecked())  R3H|= 0x4000;
    if (ui->R3_31->isChecked())  R3H|= 0x8000;

    x.sprintf("%04X", R3H); ui->Row3H->setText(x);
    x.sprintf("%04X", R3L); ui->Row3L->setText(x);

    DataBuffer[bitmap +12] =(R3H>>8)&0xff;  DataBuffer[bitmap +13]=R3H&0xff;
    DataBuffer[bitmap +14] =(R3L>>8)&0xff;  DataBuffer[bitmap +15]=R3L&0xff;


}

void MainWindow::HexDumpBuffer()
{
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int i;
    ee << "0xF0000 = "; for (i=0;i<16;i++) { x.sprintf("%02X ",DataBuffer[i+0x00]);  ee << x; } ee << "\n";
    ee << "0xF0010 = "; for (i=0;i<16;i++) { x.sprintf("%02X ",DataBuffer[i+0x10]);  ee << x; } ee << "\n";
    ee << "0xF0020 = "; for (i=0;i<16;i++) { x.sprintf("%02X ",DataBuffer[i+0x20]);  ee << x; } ee << "\n";
    ee << "0xF0030 = "; for (i=0;i<16;i++) { x.sprintf("%02X ",DataBuffer[i+0x30]);  ee << x; } ee << "\n";
    ui->plainTextEdit->appendPlainText(eeMsg);

}

void MainWindow::ReadDeviceRDSaddress ()
{
    // read RDS device ADDR before re-flashing so address can be preserved.

    Comm::ErrorCode result;
    DeviceData::MemoryRange deviceRange, hexRange;
    bool failureDetected = false;
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);


    if(!comm->isConnected())
    {
        failed = -1;
        qWarning("Device not connected");
        return;
    }

    result = comm->GetData(0xF00000,40,1,1,0xF00040,DataBuffer);

    if(result != Comm::Success)
    {
        failureDetected = true;
        qWarning("Error reading device.");

    }
    else
    {

        ADDR = DataBuffer[eemap_device_serial ]*256+DataBuffer[eemap_device_serial+1 ];
        x.sprintf("%04X ",ADDR);
        ee << "Reading RDS ADDR="; ee << x; ee << "\n";
        ui->plainTextEdit->appendPlainText(eeMsg);

    }
}

void MainWindow::on_actionReadEEPROM_triggered()
{
    Comm::ErrorCode result;
    DeviceData::MemoryRange deviceRange, hexRange;
    bool failureDetected = false;
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int i;

    if(!comm->isConnected())
    {
        failed = -1;
        qWarning("Device not connected");
        return;
    }
    ui->plainTextEdit->appendPlainText("Reading EEPROM...");

    result = comm->GetData(0xF00000,40,1,1,0xF00040,DataBuffer);

    if(result != Comm::Success)
    {
        failureDetected = true;
        qWarning("Error reading device.");

    }
    else
    {
        ui->plainTextEdit->appendPlainText("EEPROM Read Successfully\n");

        HexDumpBuffer();
        CopyBufferToScreen();
        UpdateNumberOfTones();
    }
}

void MainWindow::on_actionWriteEEPROM_triggered()
{
    Comm::ErrorCode result;
    DeviceData::MemoryRange deviceRange, hexRange;
    bool failureDetected = false;
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int i;

    if(!comm->isConnected())
    {
        failed = -1;
        qWarning("Device not connected");
        return;
    }

    ui->plainTextEdit->appendPlainText("Writing EEPROM...");

    //  copy screen to DataBuffer
    ScreenToBuffer();

    // program DataBuffer to eeprom
    result = comm->Program(0xF00000,40,1,1,Device::PIC18,0XF00040, DataBuffer);

    // read back
    ui->plainTextEdit->appendPlainText("Verifying EEPROM...");

    result = comm->GetData(0xF00000,40,1,1,0xF00040,DataBuffer);

    if(result != Comm::Success)
    {
        failureDetected = true;
        qWarning("Error reading device.");
    }
    else
    {
        ui->plainTextEdit->appendPlainText("EEPROM Written Successfully\n");
        HexDumpBuffer();
        CopyBufferToScreen();
    }
}

void MainWindow::on_actionReadFile_triggered()
{
    QString newFileName;

    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int i;

    //Create an open file dialog box, so the user can select a .hex file.
    newFileName = QFileDialog::getOpenFileName(this, "Open Eeprom File", ".", "EEP Files (*.eep)");

    if(newFileName.isEmpty())
    {
        ee << "Failed, file is Empty?";
        ui->plainTextEdit->appendPlainText(eeMsg);
        return;
    }


    QFile eehex(newFileName);

    if(!eehex.open(QIODevice::ReadOnly)) {
        ee << "Failed, file not open?";
        ui->plainTextEdit->appendPlainText(eeMsg);
        return;
    }

    qint64 bufsize = 1024;
    char *buf = new char[bufsize];
    qint64 dataSize;
    dataSize = eehex.read( buf, 0x40);
    for (i=0;i<0x40;i++) { DataBuffer[i]=buf[i]; }
    eehex.close();

    ui->plainTextEdit->appendPlainText("EEPROM Read Successfully\n");
    CopyBufferToScreen();
    HexDumpBuffer();

}

void MainWindow::on_actionSaveFile_triggered()
{
    QString newFileName;
    QString x;
    QString eeMsg;
    QTextStream ee(&eeMsg);
    int i;

    ScreenToBuffer();

    //Create an open file dialog box, so the user can select a .hex file.
    newFileName = QFileDialog::getSaveFileName(this, "Save Eeprom File", ".", "eep Files (*.eep)");

    QFile eehex(newFileName);

    if(!eehex.open(QIODevice::WriteOnly)) {
        ee << "Failed, can't open file for writing?";
        ui->plainTextEdit->appendPlainText(eeMsg);
        return;
    }

    qint64 bufsize = 1024;
    char *buf = new char[bufsize];
    qint64 dataSize;

    for (i=0;i<0x40;i++) { buf[i]=DataBuffer[i]; }

    dataSize = eehex.write( buf, 0x40);
    eehex.close();

    ui->plainTextEdit->appendPlainText("File Saved Successfully\n");
    HexDumpBuffer();
}

void MainWindow::on_actionResetButton_triggered()
{

    if(!comm->isConnected())
    {
        failed = -1;
        qWarning("Reset not sent, device not connected");
        return;
    }

    ui->plainTextEdit->appendPlainText("Resetting...");
    comm->Reset();

}

void MainWindow::on_actionRadioButtonBlink_triggered()
{
    ui->plainTextEdit->appendPlainText("Blink Selected\n");
}

void MainWindow::on_actionRadioButtonBreath_triggered()
{
    ui->plainTextEdit->appendPlainText("BreathSelected\n");
}

void MainWindow::on_actionRadioButtonSparkle_triggered()
{
    ui->plainTextEdit->appendPlainText("Sparkle Selected\n");
}

void MainWindow::on_actionRadioButtonTwinkle_triggered()
{
    ui->plainTextEdit->appendPlainText("Twinkle Selected\n");
}

void MainWindow::on_actionRadioButtonToneEnable_triggered()
{
    ui->plainTextEdit->appendPlainText("Tone Enable Selected\n");
}

void MainWindow::on_actionRadioToneDecodeDisabled_triggered()
{
    ui->plainTextEdit->appendPlainText("Tone Disable Selected\n");
}


void MainWindow::on_actionRadioSignalStrength_triggered()
{
    ui->plainTextEdit->appendPlainText("Signal Strength Mode\n");
}

void MainWindow::on_actionRadioButtonPWMDisable_triggered()
{
     ui->plainTextEdit->appendPlainText("ON/OFF Mode Selected\n");
}

void MainWindow::on_actionRadioButtonPWMEnable_triggered()
{
    ui->plainTextEdit->appendPlainText("PWM Mode Selected\n");
}

void MainWindow::on_actionRadioButtonRGBEnable_triggered()
{
    ui->plainTextEdit->appendPlainText("RGB Mode Selected\n");
}

void MainWindow::UpdateNumberOfTones()
{
    QString x;
    bool ok;
    int n,f1,f2,f3,f4,f5,f6;

    n=0;

    f1 = ui->Tone1->text().toInt(&ok,10); if (f1) n++;
    f2 = ui->Tone2->text().toInt(&ok,10); if (f2) n++;
    f3 = ui->Tone3->text().toInt(&ok,10); if (f3) n++;
    f4 = ui->Tone4->text().toInt(&ok,10); if (f4) n++;
    f5 = ui->Tone5->text().toInt(&ok,10); if (f5) n++;
    f6 = ui->Tone6->text().toInt(&ok,10); if (f6) n++;

    x.sprintf("%d",n); ui->NumTones->setText(x);

    NumTones = ui->NumTones->text().toInt(&ok,10);
    N = ui->N->text().toInt(&ok,10);

    SampleTime = (N * 1000)/37500;
    CycleTime = NumTones * SampleTime;
    FreqSpacing = 1733/SampleTime;

    x.sprintf("%d",FreqSpacing); ui->FreqSpacing->setText(x);
    x.sprintf("%d",SampleTime); ui->SampleTime->setText(x);
    x.sprintf("%d",CycleTime); ui->CycleTime->setText(x);

}


void MainWindow::on_FreqSpacing_returnPressed()
{
    QString x;
    bool ok;

    NumTones = ui->NumTones->text().toInt(&ok,10);
    FreqSpacing = ui->FreqSpacing->text().toInt(&ok,10);

    N = 65000/FreqSpacing;
    SampleTime = (N * 1000)/37500;
    CycleTime=SampleTime*NumTones;

    x.sprintf("%d",N); ui->N->setText(x);
    x.sprintf("%d",FreqSpacing); ui->FreqSpacing->setText(x);
    x.sprintf("%d",SampleTime); ui->SampleTime->setText(x);
    x.sprintf("%d",CycleTime); ui->CycleTime->setText(x);

}


void MainWindow::on_CycleTime_returnPressed()
{
    QString x;
    bool ok;

    NumTones = ui->NumTones->text().toInt(&ok,10);
    CycleTime = ui->CycleTime->text().toInt(&ok,10);

    SampleTime = CycleTime/NumTones;
    N = SampleTime*37500/1000;
    FreqSpacing = 65000/N;

    x.sprintf("%d",N); ui->N->setText(x);
    x.sprintf("%d",FreqSpacing); ui->FreqSpacing->setText(x);
    x.sprintf("%d",SampleTime); ui->SampleTime->setText(x);

}


void MainWindow::on_Tone1_returnPressed()
{
    UpdateNumberOfTones();

}

void MainWindow::on_Tone2_returnPressed()
{
    UpdateNumberOfTones();

}

void MainWindow::on_Tone3_returnPressed()
{
    UpdateNumberOfTones();

}

void MainWindow::on_Tone4_returnPressed()
{
    UpdateNumberOfTones();

}

void MainWindow::on_Tone5_returnPressed()
{
    UpdateNumberOfTones();

}

void MainWindow::on_Tone6_returnPressed()
{
    UpdateNumberOfTones();

}


void MainWindow::on_actionLSE_Help_Resources_triggered()
{
    QString msg;
    QTextStream stream(&msg);

    QMessageBox help(this);

    help.setWindowTitle("Linzer Schnitte Help Resources");
    help.setTextFormat(Qt::RichText);

    stream << "LS 2014 " << VERSION << "<br>";
    stream << "<br>";
    stream << "Linzer Schnitte Help Resources Source Files and Howto videos can be accessed at the following locations<br><br>";
    stream << "<a href=\"http://www.aec.at/linzerschnitte\">http://www.aec.at/linzerschnitte</a><br><br>";
    stream << "<a href=\"https://github.com/RayGardiner/LinzerSchnitte-\">https://github.com/RayGardiner/LinzerSchnitte-</a><br><br>";
    stream << "<a href=\"https://github.com/NeuralSpaz\">https://github.com/NeuralSpaz</a><br><br>";

    help.setText(msg);
    help.exec();

}
