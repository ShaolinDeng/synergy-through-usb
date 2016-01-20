/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2012 Bolton Software Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IARCHUSBDATALINK_H
#define IARCHUSBDATALINK_H

#include "common/IInterface.h"

typedef struct libusb_context* USBContextHandle;
typedef struct libusb_device* USBDeviceEnumerator;
typedef struct libusb_device_handle* USBDeviceHandle;



struct USBDeviceInfo
{
	unsigned short idVendor;
	unsigned short idProduct;
	unsigned char busNumber;
	unsigned char devAddress;
};

struct USBDataLinkConfig: USBDeviceInfo
{
	int ifid;
	int bulkin;
	int bulkout;
};


//! Interface for architecture dependent USB
/*!
This interface defines the USB transport operations required by
synergy.  Each architecture must implement this interface.
*/
class IArchUsbDataLink : public IInterface {
public:
	virtual void init() = 0;

	virtual void usbInit() = 0;
	virtual void usbShut() = 0;
	virtual USBContextHandle usbGetContext() = 0;

	// returns null-terminated list of USB devices
	virtual void usbGetDeviceList(USBDeviceEnumerator **list) = 0;
	virtual void usbFreeDeviceList(USBDeviceEnumerator *list) = 0;
	virtual void usbGetDeviceInfo(USBDeviceEnumerator devEnum, struct USBDeviceInfo &info) = 0;

	virtual USBDeviceHandle usbOpenDevice(USBDeviceEnumerator devEnum, int ifid) = 0;
	virtual USBDeviceHandle usbOpenDevice(struct USBDeviceInfo &devInfo, int ifid) = 0;
	virtual void usbCloseDevice(USBDeviceHandle dev, int ifid) = 0;
	virtual int usbBulkTransfer(USBDeviceHandle dev, bool write, unsigned char port, char* buf, unsigned int len, unsigned int timeout) = 0;
	virtual int usbTryBulkTransfer(USBDeviceHandle dev, bool write, unsigned char port, char* buf, unsigned int len, unsigned int timeout) = 0;
};

#endif
