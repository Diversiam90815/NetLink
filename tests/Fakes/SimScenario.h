/*
  ==============================================================================
	Module:         SimScenario
	Description:    Engines on one fake network, run by a SimDriver: time only
					passes when the test lets it
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "EngineHarness.h"
#include "FakeDatagramNetwork.h"
#include "SimDriver.h"


namespace FakeNet
{

struct SimScenario
{
	struct Node
	{
		Node(std::string name, const std::string &ip) : name(std::move(name)), ip(ip), iface(ip) {}

		netlink::PeerId id() const { return engine->id(); }

		// The application takes nothing anymore: what arrives keeps waiting for it until it is released
		void			hold() { holding = true; }

		void			release()
		{
			holding = false;
			held.clear();
		}

		// ... or takes that many bytes of what is waiting, the oldest first
		void consume(const size_t bytes)
		{
			allowance += bytes;

			while (!held.empty() && held.front().first <= allowance)
			{
				allowance -= held.front().first;
				held.pop_front();
			}

			if (held.empty())
				allowance = 0;
		}

		std::string																	  name;
		std::string																	  ip;
		TestInterface																  iface;
		EventRecorder																  events;
		std::vector<std::chrono::microseconds>										  received;		   // when each message arrived
		bool																		  recording{true}; // false: messages are counted in `received`, their payload is not kept
		std::function<void(netlink::PeerId, netlink::Lane, const netlink::Message &)> onMessage;	   // what the application does with a message
		bool																		  holding{false};
		size_t																		  allowance{0};
		std::deque<std::pair<size_t, std::shared_ptr<void>>>						  held;			   // payload bytes of a batch, and what keeps them counted as waiting
		std::unique_ptr<netlink::NetworkEngine>										  engine;		   // last: gone first
	};

	static netlink::EngineConfig defaultConfig()
	{
		netlink::EngineConfig config;
		config.appId	  = "sim";
		config.appVersion = "1.0";
		return config;
	}

	// An engine of its own machine with that address
	Node &add(const std::string &name, const std::string &ip, const netlink::EngineConfig &config = defaultConfig()) { return add(name, ip, network->factory(ip), config); }

	// ... or on a machine that is described by its socket factory
	Node &add(const std::string &name, const std::string &ip, DatagramSocketFactory factory, netlink::EngineConfig config = defaultConfig())
	{
		Node &node		   = *nodes.emplace_back(std::make_unique<Node>(name, ip));
		config.displayName = name;
		node.engine		   = std::make_unique<netlink::NetworkEngine>(config, std::move(factory), node.iface.provider());
		node.engine->setAnnouncing(true);

		driver.add(*node.engine,
				   [this, &node](netlink::EventBatch &&batch)
				   {
					   size_t bytes = 0;

					   for (auto &event : batch.events)
					   {
						   if (event.kind == netlink::EngineEvent::Kind::Message)
						   {
							   auto message = event.takeMessage();
							   event.media.reset();

							   // A Media message that newer ones replaced before the application got to it
							   if (!message)
								   continue;

							   node.received.push_back(driver.elapsed());
							   bytes += event.lane != netlink::Lane::Media ? message->data.size() : 0;

							   if (node.onMessage)
								   node.onMessage(event.peer, event.lane, *message);

							   if (!node.recording)
								   continue;

							   event.message = std::move(*message);
						   }

						   node.events.record(event);
					   }

					   if (node.holding && batch.backlog)
						   node.held.emplace_back(bytes, std::move(batch.backlog));
				   });

		return node;
	}

	// The node's machine freezes: nothing is sent or answered from there anymore
	void freeze(const Node &node) { driver.remove(*node.engine); }

	bool discover(Node &a, Node &b, const std::chrono::microseconds limit = std::chrono::seconds{10})
	{
		return driver.runUntil([&] { return a.events.knows(b.id()) && b.events.knows(a.id()); }, limit);
	}

	// a asks b for a session and both report it
	bool connect(Node &a, Node &b, const std::chrono::microseconds limit = std::chrono::seconds{10})
	{
		if (!discover(a, b, limit) || !a.engine->connect(b.id()))
			return false;

		return driver.runUntil([&] { return a.events.isConnectedTo(b.id()) && b.events.isConnectedTo(a.id()); }, limit);
	}

	std::shared_ptr<FakeDatagramNetwork> network = FakeDatagramNetwork::create();
	SimDriver							 driver{network};
	std::vector<std::unique_ptr<Node>>	 nodes; // after the driver: the engines go first
};

} // namespace FakeNet
