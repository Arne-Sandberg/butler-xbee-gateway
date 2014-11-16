/*
 *******************************************************************************
 *
 * Purpose: Routing between networks
 *
 *******************************************************************************
 * Copyright Monstrenyatko 2014.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

/* Internal Includes */
#include "Router.h"
#include "NetworkingDataUnit.h"
#include "CommandProcessor.h"
#include "Error.h"
#include "Application.h"
#include "XBeeNet.h"
#include "SerialPort.h"
/* External Includes */
/* System Includes */
#include <iostream>

///////////////////// RouterContext /////////////////////
struct RouterContext {
	Utils::CommandProcessor							processor;
};

///////////////////// RouterCommands /////////////////////
class RouterCommands: public Utils::Command {
public:
	virtual ~RouterCommands() {}
};

class RouterCommandsProcess: public RouterCommands {
public:
	typedef std::function<void(std::unique_ptr<Networking::DataUnit>)> Cbk;
	RouterCommandsProcess(Cbk cbk, std::unique_ptr<Networking::DataUnit> data)
	:
		mCbk(cbk),
		mData(std::move(data))
	{}

	void execute() {
		mCbk(std::move(mData));
	}
private:
	Cbk											mCbk;
	std::unique_ptr<Networking::DataUnit>		mData;
};

///////////////////// Router /////////////////////
Router::Router()
:
	mCtx(new RouterContext)
{}

Router::~Router() {
	delete mCtx;
}

void Router::start() {
	mCtx->processor.start();
}

void Router::stop() {
	mCtx->processor.stop();
}

void Router::process(std::unique_ptr<Networking::DataUnit> unit) {
	std::unique_ptr<Utils::Command> cmd (new RouterCommandsProcess(
		[this] (std::unique_ptr<Networking::DataUnit> a) {
				onProcess(std::move(a));
		},
		std::move(unit)
	));
	mCtx->processor.process(std::move(cmd));
}

///////////////////// Router::Internal /////////////////////
void Router::onProcess(std::unique_ptr<Networking::DataUnit> unit) {
	try {
		try {
			switch(unit->getOrigin()) {
				case Networking::Origin::SERIAL:
				{
					Networking::DataUnitSerial* u = dynamic_cast<Networking::DataUnitSerial*>(unit.get());
					if (!u) {
						throw Utils::Error("Wrong unit type");
					}
					Application::get().getXBeeNet().from(u->popData());
				}
					break;
				case Networking::Origin::XBEE_ENCODER:
				{
					Networking::DataUnitXBeeEncoder* u = dynamic_cast<Networking::DataUnitXBeeEncoder*>(unit.get());
					if (!u) {
						throw Utils::Error("Wrong unit type");
					}
					Application::get().getSerial().write(u->popData());
				}
					break;
				case Networking::Origin::XBEE_NET:
				{

				}
				case Networking::Origin::TCP:
				default:
					throw Utils::Error("Origin is not implemented");
			}
		} catch (std::exception& e) {
			throw Utils::Error(e, UTILS_STR_CLASS_FUNCTION(Router));
		}
	} catch (std::exception& e) {
		std::cerr<<"Routing, error: "<<e.what()<<std::endl;
	}
}
