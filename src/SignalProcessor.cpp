/*
 *******************************************************************************
 *
 * Purpose: Processor of the system signals.
 *
 *******************************************************************************
 * Copyright Monstrenyatko 2014.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

/* Internal Includes */
#include "SignalProcessor.h"
#include "CommandProcessor.h"
#include "Commands.h"
#include "Application.h"
#include "Error.h"
#include "Thread.h"
/* External Includes */
/* System Includes */
#include <boost/asio/io_service.hpp>
#include <boost/asio/signal_set.hpp>
#include <iostream>


///////////////////// SignalListener /////////////////////
class SignalListener: private Utils::Thread {
public:
	SignalListener():
		mSigSet(mIoService, SIGINT, SIGTERM)
	{
	}

	void start(void) {
		Utils::Thread::start();
	}

	void stop(void) {
		Utils::Thread::stop();
	}
private:
	// Objects
	boost::asio::io_service							mIoService;
	boost::asio::signal_set							mSigSet;

	// Methods
	void onStarted() {
		schedule();
	}

	void onStop() {
		try {
			mSigSet.cancel();
			mSigSet.clear();
			mIoService.stop();
			mIoService.reset();
		} catch (boost::system::system_error& e) {
			// ignore
		}
	}

	void schedule() {
		mSigSet.async_wait([this](const boost::system::error_code& a, int b) {
			onSignal(a, b);
		});
	}

	void loop() {
		while (isAlive()) {
			try {
				mIoService.reset();
				mIoService.run_one();
			} catch (boost::system::system_error& e) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}

	void onSignal(const boost::system::error_code& error, int sigNumber)
	{
		if (error==boost::asio::error::operation_aborted) {
			// Signal set is cancelled
		} else {
			switch(sigNumber) {
				case SIGINT:
				case SIGTERM:
				{
					// Signal to stop
					std::unique_ptr<Utils::Command> cmd (new CommandApplicationStop);
					Application::get().getProcessor().process(std::move(cmd));
				}
					break;
				default:
					std::cerr<<UTILS_STR_CLASS_FUNCTION(SignalProcessor)
						<<", not handled signal: "<<sigNumber<<std::endl;
					break;
			}
		}
	}
};

///////////////////// SignalProcessor /////////////////////
SignalProcessor::SignalProcessor()
:
	mSignalListener(new SignalListener)
{
}

SignalProcessor::~SignalProcessor()
throw()
{
	stop();
	delete mSignalListener;
}

void SignalProcessor::start(void) {
	mSignalListener->start();
}

void SignalProcessor::stop(void) {
	mSignalListener->stop();
}
