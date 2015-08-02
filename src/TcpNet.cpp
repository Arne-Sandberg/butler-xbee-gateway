/*
 *******************************************************************************
 *
 * Purpose: TCP network implementation
 *
 *******************************************************************************
 * Copyright Monstrenyatko 2014.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

/* Internal Includes */
#include "TcpNet.h"
#include "TcpNetDb.h"
#include "TcpNetConnection.h"
#include "TcpNetCommand.h"
#include "NetworkingAddress.h"
#include "Application.h"
#include "CommandProcessor.h"
#include "Memory.h"
#include "Thread.h"
/* External Includes */
/* System Includes */
#include <boost/asio.hpp>
#include <assert.h>

///////////////////// TcpNetIoService /////////////////////
class TcpNetIoService: private Utils::Thread {
public:
	void start(void) {
		Utils::Thread::start();
	}

	void stop(void) {
		Utils::Thread::stop();
	}

	boost::asio::io_service& getIoService() {return mIoService;}
private:
	// Objects
	boost::asio::io_service							mIoService;

	// Methods
	void onStop() {
		mIoService.stop();
	}

	void loop() {
		while (isAlive()) {
			try {
				mIoService.reset();
				mIoService.run();
			} catch (boost::system::system_error& e) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}
};

///////////////////// TcpNetContext /////////////////////
struct TcpNetContext {
	Utils::CommandProcessor							processor;
	TcpNetIoService									ioService;
	TcpNetDb										db;
};

///////////////////// TcpNetCommands /////////////////////
class TcpNetCommandSend: public TcpNetCommand {
public:
	typedef std::function<void(std::unique_ptr<Networking::Address>,
			std::unique_ptr<Networking::Address>,
			std::unique_ptr<Networking::Buffer>)> Cbk;
	TcpNetCommandSend(TcpNet& owner, Cbk cbk,
			const Networking::Address* from, const Networking::Address* to,
			std::unique_ptr<Networking::Buffer> buffer)
	:
		TcpNetCommand(owner),
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

///////////////////// TcpNet /////////////////////
TcpNet::TcpNet()
:
	mLog(__FUNCTION__),
	mCtx(new TcpNetContext)
{
}

TcpNet::~TcpNet() {
	delete mCtx;
}

void TcpNet::start() {
	mCtx->processor.start();
	mCtx->ioService.start();
}

void TcpNet::stop() {
	mCtx->ioService.stop();
	mCtx->processor.stop();
}

void TcpNet::send(const Networking::Address* from, const Networking::Address* to,
			std::unique_ptr<Networking::Buffer> buffer)
throw ()
{
	assert(from);
	assert(to);
	assert(to->getOrigin()==Networking::Origin::TCP);
	assert(buffer.get());

	std::unique_ptr<Utils::Command> cmd (new TcpNetCommandSend(*this,
		[this] (std::unique_ptr<Networking::Address> a, std::unique_ptr<Networking::Address> b,
				std::unique_ptr<Networking::Buffer> c) {
				onSend(std::move(a), std::move(b), std::move(c));
		},
		from,
		to,
		std::move(buffer)
	));
	mCtx->processor.process(std::move(cmd));
}

///////////////////// TcpNet::Internal /////////////////////
void TcpNet::onSend(std::unique_ptr<Networking::Address> from, std::unique_ptr<Networking::Address> to,
		std::unique_ptr<Networking::Buffer> buffer)
{
	*mLog.debug() << UTILS_STR_FUNCTION << ", size:" << buffer->size();
	try {
		TcpNetConnection* connection = mCtx->db.get(*from, *to);
		if (!connection) {
			// prepare parameters
			std::unique_ptr<Networking::AddressTcp> to_ =
					Utils::dynamic_unique_ptr_cast<Networking::AddressTcp, Networking::Address>(to);
			assert(to_.get());
			// create new
			std::unique_ptr<TcpNetConnection> t(new TcpNetConnection
					(*this, std::move(from), std::move(to_)));
			connection = t.get();
			mCtx->db.put(std::move(t));
		}
		// send
		connection->send(std::move(buffer));
		*mLog.debug() << UTILS_STR_FUNCTION << ", push to ID: " << connection->getId();
	} catch (Utils::Error& e) {
		*mLog.error() << UTILS_STR_FUNCTION << ", error: " << e.what();
	}
}

///////////////////// TcpNet::Internal Interface /////////////////////
boost::asio::io_service& TcpNet::getIo() const {
	return mCtx->ioService.getIoService();
}

Utils::CommandProcessor& TcpNet::getProcessor() const {
	return mCtx->processor;
}

TcpNetDb& TcpNet::getDb() const {
	return mCtx->db;
}
