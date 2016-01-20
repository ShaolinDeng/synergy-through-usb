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

#include "arch/Arch.h"
#include "arch/XArch.h"
#include "mt/Lock.h"
#include "net/XSocket.h"
#include "CUSBDataLink.h"
#include "CUSBAddress.h"

const char*		kUsbConnect	= "USB_CONNECT";
const char*		kUsbAccept	= "USB_ACCEPT";
const char*		kUsbReject	= "USB_REJECT";

enum message_id {
	MSGID_NORMAL = 0,
	MSGID_DISCONNECT = 1
};

struct message_hdr {
	message_id		id;
	unsigned int	data_size;
};

//
// CUSBDataLink
//

CUSBDataLink::CUSBDataLink(IEventQueue* events) :
	base_t(events),
	m_events(events),
	m_device(NULL),
	m_transferRead(NULL),
	m_transferWrite(NULL),
	m_mutex(),
	m_flushed(&m_mutex, true),
	m_connected(false),
	m_readable(false),
	m_writable(false),
	//m_acceptedMutex(),
	m_acceptedFlag(&m_mutex, false),
	//m_activeTransfersMutex(),
	m_activeTransfers(&m_mutex, 0)
{
	memset(&m_config, 0, sizeof(m_config));
}

CUSBDataLink::~CUSBDataLink()
{
	m_writable = false;
	try {
		close();
	} 
	catch (...) {
		// ignore
	}
}

//
// IDataTransfer overrides
//
void
CUSBDataLink::connect(const BaseAddress& addr)
{
	close();
	
	initConnection(addr);
	
	std::string buf(kUsbConnect);
	write(buf.c_str(), buf.size());	
	
	{
		Lock lock(&m_mutex);
		
		// wait for connection to be accepted
		while (m_acceptedFlag == false) {
			m_acceptedFlag.wait();
		}
		
		if (!m_connected) {
			throw XSocketConnect("connection failed");
		}

		sendEvent(m_events->forIDataSocket().connected());
	}
}

//
// ISocket overrides
//

void
CUSBDataLink::bind(const BaseAddress & addr)
{
	initConnection(addr);
	{
		Lock lock(&m_mutex);
		// set accepted flag, because this is a listen socket.
		m_acceptedFlag = true;
		m_connected = true;
	}
}

void
CUSBDataLink::close()
{
	Lock lock(&m_mutex);

	// clear buffers and enter disconnected state
	if (m_connected) {

		if (m_writable)
		{
			// cleanup output buffer
			m_outputBuffer.pop(m_outputBuffer.getSize());

			// send disconnect message
			message_id msg_id = MSGID_DISCONNECT;
			m_outputBuffer.write(&msg_id, sizeof(msg_id));
			scheduleWrite();

			// wait for message to be delivered
			while (m_flushed == false) {
				m_flushed.wait();
			}
		}

		sendEvent(m_events->forISocket().disconnected());
	}
	onDisconnected();

	// cancel active transfers
	if (m_transferRead) {
		libusb_cancel_transfer(m_transferRead);
	}

	if (m_transferWrite) {
		libusb_cancel_transfer(m_transferWrite);
	}
	
	// wait for transfers to finish
	while (m_activeTransfers > 0) {
		m_activeTransfers.wait();
	}
	
	if (m_transferRead)	{
		libusb_free_transfer(m_transferRead);
		m_transferRead = NULL;
	}

	if (m_transferWrite) {
		libusb_free_transfer(m_transferWrite);
		m_transferWrite = NULL;
	}

	// close the usb device
	if (m_device) {
		ARCH->usbCloseDevice(m_device, m_config.ifid);
		m_device = NULL;
	}
}

void*
CUSBDataLink::getEventTarget() const
{
	return const_cast<void*>(reinterpret_cast<const void*>(this));
}

//
// IStream overrides
//

bool
CUSBDataLink::isReady() const
{
	Lock lock(&m_mutex);
	return (m_inputBuffer.getSize() > 0);
}

UInt32
CUSBDataLink::getSize() const
{
	Lock lock(&m_mutex);
	return m_inputBuffer.getSize();
}

UInt32
CUSBDataLink::read(void* buffer, UInt32 n)
{
	Lock lock(&m_mutex);

	// copy data directly from our input buffer
	UInt32 size = m_inputBuffer.getSize();
	if (n > size) {
		n = size;
	}
	if (buffer != NULL && n != 0) {
		memcpy(buffer, m_inputBuffer.peek(n), n);
	}
	m_inputBuffer.pop(n);

	// if no more data and we cannot read or write then send disconnected
	if (n > 0 && m_inputBuffer.getSize() == 0 && !m_readable && !m_writable) {
		sendEvent(m_events->forISocket().disconnected());
		m_connected = false;
	}

	return n;
}

void
CUSBDataLink::write(const void* buffer, UInt32 n)
{
	bool wasEmpty;
	
	Lock lock(&m_mutex);

	// must not have shutdown output
	if (!m_writable) {
		sendEvent(m_events->forIStream().outputError());
		return;
	}

	// ignore empty writes
	if (n == 0) {
		return;
	}

	// copy data to the output buffer
	wasEmpty = (m_outputBuffer.getSize() == 0);

	message_hdr hdr;

	hdr.id = MSGID_NORMAL;
	hdr.data_size = n;
	m_outputBuffer.write(&hdr, sizeof(hdr));
	m_outputBuffer.write(buffer, n);

	assert(m_outputBuffer.getSize() <= sizeof(m_readBuffer));

	// there's data to write
	m_flushed = false;

	if (wasEmpty)
		scheduleWrite();
}

void
CUSBDataLink::flush()
{
	Lock lock(&m_mutex);
	
	while (m_flushed == false) {
		m_flushed.wait();
	}
}

void
CUSBDataLink::shutdownInput()
{
	Lock lock(&m_mutex);

	// shutdown buffer for reading
	if (m_readable) {
		sendEvent(m_events->forIStream().inputShutdown());
		onInputShutdown();
	}
}

void
CUSBDataLink::shutdownOutput()
{
	Lock lock(&m_mutex);

	// shutdown buffer for writing
	if (m_writable) {
		sendEvent(m_events->forIStream().outputShutdown());
		onOutputShutdown();
	}
}

//
// private
//

void
CUSBDataLink::initConnection(const BaseAddress& addr)
{
	Lock lock(&m_mutex);

	assert(addr.getAddressType() == BaseAddress::USB);
	const CUSBAddress& usbAddress = reinterpret_cast<const CUSBAddress&>(addr);

	m_config.idVendor = usbAddress.GetVID();
	m_config.idProduct = usbAddress.GetPID();
	m_config.busNumber = usbAddress.GetIDBus();
	m_config.devAddress = usbAddress.GetIDDeviceOnBus();
	m_config.ifid = 0;
	m_config.bulkin = usbAddress.GetIDBulkIN();
	m_config.bulkout = usbAddress.GetIDBulkOut();

	try 
	{
		m_device = ARCH->usbOpenDevice(m_config, m_config.ifid);	
		if (!m_device) {
			throw XSocketConnect("Open device failed");
		}

		m_transferRead = libusb_alloc_transfer(0);
		if (!m_transferRead) {
			throw XSocketConnect("alloc read transfer failed");
		}

		m_transferWrite = libusb_alloc_transfer(0);
		if (!m_transferWrite) {
			throw XSocketConnect("alloc write transfer failed");
		}
	} 
	catch (...) 
	{
		if (m_transferRead)	{
			libusb_free_transfer(m_transferRead);
			m_transferRead = NULL;
		}

		if (m_transferWrite) {
			libusb_free_transfer(m_transferWrite);
			m_transferWrite = NULL;
		}

		// close the usb device
		if (m_device) {
			ARCH->usbCloseDevice(m_device, m_config.ifid);
			m_device = NULL;
		}

		throw;
	}

	m_readable = true;
	m_writable = true;

	// Start pooling input endpoint.
	m_activeTransfers = m_activeTransfers + 1;
	m_transferRead->status = LIBUSB_TRANSFER_COMPLETED;
	m_transferRead->actual_length = 0;
	m_transferRead->user_data = this;
	readCallback(m_transferRead);
}

void CUSBDataLink::scheduleWrite()
{
	// No active write transfers at this moment. Call writeCallback to start new write transfer.
	m_activeTransfers = m_activeTransfers + 1;

	m_transferWrite->status = LIBUSB_TRANSFER_COMPLETED;
	m_transferWrite->actual_length = 0;
	m_transferWrite->user_data = this;
	writeCallback(m_transferWrite);
}

void CUSBDataLink::readCallback(libusb_transfer *transfer)
{
	CUSBDataLink* this_ = reinterpret_cast<CUSBDataLink*>(transfer->user_data);

	Lock lock(&this_->m_mutex);

	if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
	{
		this_->sendEvent(this_->m_events->forIStream().inputShutdown());
		if (!this_->m_writable && this_->m_inputBuffer.getSize() == 0) {
			this_->sendEvent(this_->m_events->forISocket().disconnected());
			this_->m_connected = false;
		}
		this_->m_readable = false;

		return;
	}

	this_->m_activeTransfers = this_->m_activeTransfers - 1;
	if (!this_->m_readable)
	{
		this_->m_activeTransfers.signal();
		return;
	}

	size_t n = transfer->actual_length;
	if (n > 0) {
		bool wasEmpty = (this_->m_inputBuffer.getSize() == 0);
		bool notifyClients = false;

		message_hdr hdr;
		char* data_ptr = this_->m_readBuffer;

		while (n > 0) 
		{
			assert(n >= sizeof(hdr));
			
			memcpy(&hdr, data_ptr, sizeof(hdr));

			n -= sizeof(hdr);
			data_ptr += sizeof(hdr);

			assert(hdr.data_size <= n);
			this_->m_inputBuffer.write(data_ptr, hdr.data_size);

			n -= hdr.data_size;
			data_ptr += hdr.data_size;

			switch (hdr.id)
			{
			case MSGID_NORMAL: 
				break;
			case MSGID_DISCONNECT:
				assert(n == 0);
				//this_->m_connected = false;
				this_->sendEvent(this_->m_events->forIStream().inputShutdown());
				//this_->sendEvent(getDisconnectedEvent());
				break;
			default:
				assert(false);
			}

			// TODO: add processing of transfers that exceed read buffer size
			// slurp up as much as possible
			//do {
			//	m_inputBuffer.write(buffer, (UInt32)n);
			//	n = ARCH->readSocket(m_socket, buffer, sizeof(buffer));
			//} while (n > 0);
	
			if (!this_->m_acceptedFlag)
			{
				std::string buf("");
				buf.resize(this_->m_inputBuffer.getSize(), 0);
				this_->read((void*)buf.c_str(), buf.size());

				if (buf == kUsbAccept)
				{
					this_->m_connected = true;
				}

				this_->m_acceptedFlag = true;
				this_->m_acceptedFlag.broadcast();
			}
			else
			{
				// send input ready if input buffer was empty
				if (wasEmpty && hdr.data_size > 0) {
					notifyClients = true;
				}
			}
		}

		if (notifyClients) {
			this_->sendEvent(this_->m_events->forIStream().inputReady());
		}

	}

	this_->m_activeTransfers = this_->m_activeTransfers + 1;
	libusb_fill_bulk_transfer(this_->m_transferRead, this_->m_device, this_->m_config.bulkin, (unsigned char*)this_->m_readBuffer, sizeof(this_->m_readBuffer), CUSBDataLink::readCallback, this_, -1);
	libusb_submit_transfer(this_->m_transferRead);
}

void CUSBDataLink::writeCallback(libusb_transfer *transfer)
{
	CUSBDataLink* this_ = reinterpret_cast<CUSBDataLink*>(transfer->user_data);

	Lock lock(&this_->m_mutex);

	if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
	{
		this_->sendEvent(this_->m_events->forISocket().disconnected());
		this_->onDisconnected();
		return;
	}

	this_->m_activeTransfers = this_->m_activeTransfers - 1;
	if (!this_->m_writable)
	{
		this_->m_activeTransfers.signal();
		return;
	}

	// discard written data
	UInt32 n = transfer->actual_length;
	if (n > 0) 
	{
		this_->m_outputBuffer.pop(n);
	}

	n = this_->m_outputBuffer.getSize();
	if (n == 0) 
	{
		this_->sendEvent(this_->m_events->forIStream().outputFlushed());
		this_->m_flushed = true;
		this_->m_flushed.broadcast();
	}
	else
	{
		// write data
		this_->m_activeTransfers = this_->m_activeTransfers + 1;
		const void* buffer = this_->m_outputBuffer.peek(n);
		memcpy(this_->m_writeBuffer, buffer, n);
		libusb_fill_bulk_transfer(this_->m_transferWrite, this_->m_device, this_->m_config.bulkout, (unsigned char*)this_->m_writeBuffer, n, CUSBDataLink::writeCallback, this_, -1);
		libusb_submit_transfer(this_->m_transferWrite);
	}
}

void
CUSBDataLink::sendEvent(Event::Type type)
{
	m_events->addEvent(Event(type, getEventTarget(), NULL));
}

void
CUSBDataLink::onInputShutdown()
{
	m_inputBuffer.pop(m_inputBuffer.getSize());
	m_readable = false;
}

void
CUSBDataLink::onOutputShutdown()
{
	m_outputBuffer.pop(m_outputBuffer.getSize());
	m_writable = false;

	// we're now flushed
	m_flushed = true;
	m_flushed.broadcast();
}

void
CUSBDataLink::onDisconnected()
{
	// disconnected
	onInputShutdown();
	onOutputShutdown();
	m_connected = false;
}
