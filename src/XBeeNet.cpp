/*
 *******************************************************************************
 *
 * Purpose: XBee network implementation
 *
 *******************************************************************************
 * Copyright Monstrenyatko 2014.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

/* Internal Includes */
#include "XBeeNet.h"
#include "XBeeFrame.h"
/* External Includes */
#include "Error.h"
#include "Application.h"
#include "CommandProcessor.h"
#include "Router.h"
#include "NetworkingDataUnit.h"
#include "Memory.h"
/* System Includes */
#include <assert.h>
#include <iostream>
#include <iomanip>

#define API_START_DELIM						((uint8_t) 0x7E)
#define API_ESCAPE							((uint8_t) 0x7D)
#define API_XON								((uint8_t) 0x11)
#define API_XOFF							((uint8_t) 0x13)
#define DO_API_ESCPE(x)						((x)^0x20)

///////////////////// XBeeNetFromBuffer /////////////////////
class XBeeNetFromBuffer {
public:
	typedef std::function<void(std::unique_ptr<XBeeBuffer>)> onFrame;

	XBeeNetFromBuffer(onFrame cbk):
		mOnFrameCbk(cbk),
		mBuffer(new XBeeBuffer),
		mIsEscapeSequence(false),
		mFrameLength(0)
	{
	}

	/**
	 * Copies bytes with Escapes removing
	 *
	 * @param buffer XBee network buffer
	 */
	void push(const XBeeBuffer& buffer) {
		for (uint8_t b: buffer) {
			// wait the start of the frame
			if (mBuffer->empty()) {
				switch (b) {
					case API_START_DELIM:
						mBuffer->push_back(b);
						continue;
					default:
						// skip
						continue;
				}
			}
			// process Escape sequence
			if (mIsEscapeSequence) {
				switch (b) {
					case DO_API_ESCPE(API_START_DELIM):
					case DO_API_ESCPE(API_ESCAPE):
					case DO_API_ESCPE(API_XON):
					case DO_API_ESCPE(API_XOFF):
						mIsEscapeSequence = false;
						mBuffer->push_back(DO_API_ESCPE(b));
						continue;
					default:
						std::cerr<<UTILS_STR_CLASS_FUNCTION(XBeeNetFromBuffer)
							<<", bad Escape"<<std::endl;
						drop();
						continue;
				}
			}
			// other characters
			switch (b) {
				case API_ESCAPE:
					mIsEscapeSequence = true;
					continue;
				case API_START_DELIM:
					std::cerr<<UTILS_STR_CLASS_FUNCTION(XBeeNetFromBuffer)
						<<", unexpected start of next frame"<<std::endl;
					drop();
					// no break
				default:
					mBuffer->push_back(b);
					break;
			}
			// get length; length is 16-bit [1:2] bytes
			if (!mFrameLength && mBuffer->size()>=3) {
				uint8_t b1 = (*mBuffer)[1];
				uint8_t b2 = (*mBuffer)[2];
				mFrameLength = ((((uint16_t)b1)<<8) & 0xFF00) | (b2 & 0x00FF);
				mBuffer->reserve(getFrameSize());
			}
			// check length
			if (mFrameLength && mBuffer->size() == getFrameSize()) {
				// frame is received
				pop();
			}
		}
	}

private:
	// Objects
	onFrame										mOnFrameCbk;
	std::unique_ptr<XBeeBuffer>					mBuffer;
	bool										mIsEscapeSequence;
	uint16_t									mFrameLength;

	/**
	 * Notifies about new received frame and starts assembling of a new one.
	 */
	void pop() {
		mOnFrameCbk(std::move(mBuffer));
		drop();
	}

	/**
	 * Drops current context.
	 */
	void drop() {
		mBuffer.reset(new XBeeBuffer);
		mIsEscapeSequence = false;
		mFrameLength = 0;
	}

	uint32_t getFrameSize() {
		uint32_t res = 0;
		if (mFrameLength) {
			res =	1					// API_START_DELIM
					+ 2					// 16-bit length
					+ mFrameLength		// Payload
					+ 1;				// Checksum
		}
		return res;
	}
};

///////////////////// XBeeNetTo /////////////////////
class XBeeNetTo {
public:
	static void addEscapes(XBeeBuffer& buffer) {
		for (auto it = buffer.begin(); it!=buffer.end(); it++) {
			switch (*it) {
				default:
					continue;
				case API_START_DELIM:
					if (it==buffer.begin()) {
						// do not escape the first byte
						continue;
					}
				// no break
				case API_ESCAPE:
				case API_XON:
				case API_XOFF:
					break;
			}
			// do escape
			it = buffer.insert(it, API_ESCAPE);
			++it;
			*it = DO_API_ESCPE(*it);
		}
	}
};

///////////////////// XBeeNetContext /////////////////////
struct XBeeNetContext {
	Utils::CommandProcessor							processor;
	std::unique_ptr<XBeeNetFromBuffer>				fromBuffer;
};

///////////////////// XBeeNetCommands /////////////////////
class XBeeNetCommand: public Utils::Command {
public:
	virtual ~XBeeNetCommand() {}
};

class XBeeNetCommandFrom: public XBeeNetCommand {
public:
	typedef std::function<void(std::unique_ptr<XBeeBuffer>)> Cbk;
	XBeeNetCommandFrom(Cbk cbk, std::unique_ptr<XBeeBuffer> data)
	:
		mCbk(cbk),
		mData(std::move(data))
	{}

	void execute() {
		mCbk(std::move(mData));
	}
private:
	Cbk								mCbk;
	std::unique_ptr<XBeeBuffer>		mData;
};

class XBeeNetCommandTo: public XBeeNetCommand {
public:
	typedef std::function<void(std::unique_ptr<Networking::Address>,
				std::unique_ptr<Networking::Address>,
				std::unique_ptr<Networking::Buffer>)> Cbk;
	XBeeNetCommandTo(Cbk cbk,
			const Networking::Address* from, const Networking::Address* to,
			std::unique_ptr<Networking::Buffer> buffer)
	:
		mCbk(cbk),
		mFrom(std::move(from->clone())),
		mTo(std::move(to->clone())),
		mData(std::move(buffer))
	{}

	void execute() {
		mCbk(std::move(mFrom), std::move(mTo), std::move(mData));
	}
private:
	Cbk											mCbk;
	std::unique_ptr<Networking::Address>		mFrom;
	std::unique_ptr<Networking::Address>		mTo;
	std::unique_ptr<Networking::Buffer>			mData;
};

///////////////////// XBeeNet /////////////////////
XBeeNet::XBeeNet()
:
	mCtx(new XBeeNetContext)
{
	mCtx->fromBuffer.reset(new XBeeNetFromBuffer( [this] (std::unique_ptr<XBeeBuffer> a) {
		onFrame(std::move(a));
	}));
}

XBeeNet::~XBeeNet() {
	delete mCtx;
}

void XBeeNet::start() {
	mCtx->processor.start();
}

void XBeeNet::stop() {
	mCtx->processor.stop();
}

void XBeeNet::from(std::unique_ptr<XBeeBuffer> buffer)
throw ()
{
	std::unique_ptr<Utils::Command> cmd (new XBeeNetCommandFrom(
		[this] (std::unique_ptr<XBeeBuffer> a) {
				onFrom(std::move(a));
		},
		std::move(buffer)
	));
	mCtx->processor.process(std::move(cmd));
}

void XBeeNet::to(const Networking::Address* from, const Networking::Address* to,
		std::unique_ptr<Networking::Buffer> buffer)
throw ()
{
	assert(from);
	assert(to);
	assert(to->getOrigin()==Networking::Origin::XBEE);
	assert(buffer.get());

	std::unique_ptr<Utils::Command> cmd (new XBeeNetCommandTo(
			[this] (std::unique_ptr<Networking::Address> a, std::unique_ptr<Networking::Address> b,
					std::unique_ptr<Networking::Buffer> c) {
					onTo(std::move(a), std::move(b), std::move(c));
			},
			from,
			to,
			std::move(buffer)
	));
	mCtx->processor.process(std::move(cmd));
}

///////////////////// XBeeNet::Internal /////////////////////
void XBeeNet::onFrom(std::unique_ptr< std::vector<uint8_t> > buffer) {
	std::ios::fmtflags f(std::cout.flags() );
	std::cout<<UTILS_STR_CLASS_FUNCTION(XBeeNet)<<", data.size:"<<buffer->size()<<std::endl;
	for (uint8_t i: *buffer) {
		std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)i<<" ";
	}
	if (!buffer->empty()) {
		std::cout << std::endl;
	}
	std::cout.flags(f);
	mCtx->fromBuffer->push(*buffer);
}

void XBeeNet::onTo(std::unique_ptr<Networking::Address> from_, std::unique_ptr<Networking::Address> to_,
		std::unique_ptr<Networking::Buffer> buffer_) {
	try {
		// get address
		std::unique_ptr<Networking::AddressXBeeNet> tTo =
				Utils::dynamic_unique_ptr_cast<Networking::AddressXBeeNet, Networking::Address>(to_);
		assert(tTo.get());
		// make frame
		XBeeFrame newframe(std::unique_ptr<XBeeFrameApiId>(new XBeeFrameApiId(XBeeFrameApiId::ZB_TX_REQ)));
		newframe.setId(std::unique_ptr<XBeeFrameId>(new XBeeFrameId(XBeeFrameId::NO_RSP)));
		newframe.setRadius(std::unique_ptr<XBeeFrameRadius>(new XBeeFrameRadius(XBeeFrameRadius::MAX)));
		newframe.setAddr64Dst(std::unique_ptr<XBeeFrameAddr64Dst>(new XBeeFrameAddr64Dst(tTo->get())));
		newframe.setAddr16Dst(std::unique_ptr<XBeeFrameAddr16Dst>(new XBeeFrameAddr16Dst(XBeeFrameAddr16Dst::UNKNOWN)));
		newframe.setOptionsSend(std::unique_ptr<XBeeFrameOptionsSend>(new XBeeFrameOptionsSend()));
		newframe.setData(std::unique_ptr<XBeeFrameData>(new XBeeFrameData(
				buffer_->begin(),
				buffer_->end())));
		std::unique_ptr<XBeeBuffer> buffer(new XBeeBuffer);
		newframe.encode(*buffer);
		{
			std::ios::fmtflags f(std::cout.flags() );
			std::cout<<"Data, size: "<<newframe.getData()->getValue().size()<<std::endl;
			std::cout<<std::hex<<std::setw(2)<<std::setfill('0');
			for (uint8_t i: newframe.getData()->getValue()) {
				std::cout<<(int)i<<" ";
			}
			std::cout<<std::endl;
			std::cout<<"NewFrame, size: "<<buffer->size()<<std::endl;
			for (uint8_t i: *buffer) {
				std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)i<<" ";
			}
			if (!buffer->empty()) {
				std::cout << std::endl;
			}
			std::cout.flags(f);
		}
		XBeeNetTo::addEscapes(*buffer);
		{
			std::ios::fmtflags f(std::cout.flags() );
			std::cout<<"NewFrame with Escapes, size: "<<buffer->size()<<std::endl;
			std::cout<<std::hex<<std::setw(2)<<std::setfill('0');
			for (uint8_t i: *buffer) {
				std::cout<<(int)i<<" ";
			}
			if (!buffer->empty()) {
				std::cout << std::endl;
			}
			std::cout.flags(f);
		}
		std::unique_ptr<Networking::DataUnit> unit(new Networking::DataUnitXBeeEncoder(
				std::move(buffer),
				std::unique_ptr<Networking::Address>
					(new Networking::AddressXBeeNet(tTo->get())),
				std::unique_ptr<Networking::Address>()
		));
		Application::get().getRouter().process(std::move(unit));
	} catch (Utils::Error& e) {
		std::cerr<<"Frame encoder, error: "<<e.what()<<std::endl;
	}
}

void XBeeNet::onFrame(std::unique_ptr<XBeeBuffer> buffer) {
	{
		std::ios::fmtflags f(std::cout.flags() );
		std::cout<<"New Frame, size:"<<buffer->size()<<std::endl;
		std::cout<<std::hex<<std::setw(2)<<std::setfill('0');
		for (uint8_t i: *buffer) {
			std::cout<<(int)i<<" ";
		}
		if (!buffer->empty()) {
			std::cout << std::endl;
		}
		std::cout.flags(f);
	}
	try {
		XBeeFrame frame(*buffer);
		{
			std::ios::fmtflags f(std::cout.flags() );
			std::cout<<std::hex<<std::setw(2)<<std::setfill('0');
			std::cout<<"Data:"<<std::endl;
			for (uint8_t i: frame.getData()->getValue()) {
				std::cout<<(int)i<<" ";
			}
			std::cout<<std::endl;
			std::cout.flags(f);
		}
		{
			std::unique_ptr<XBeeBuffer> data(new XBeeBuffer(frame.getData()->getValue()));
			std::unique_ptr<Networking::DataUnit> unit(new Networking::DataUnitXBee(
					std::move(data),
					std::unique_ptr<Networking::Address>
						(new Networking::AddressXBeeNet(frame.getAddr64Src()->getValue())),
					std::unique_ptr<Networking::Address>()
			));
			Application::get().getRouter().process(std::move(unit));
		}
	} catch (Utils::Error& e) {
		std::cerr<<"Frame parser, error: "<<e.what()<<std::endl;
	}
}
