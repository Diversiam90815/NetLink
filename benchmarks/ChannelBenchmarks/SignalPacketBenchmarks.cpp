/*
  ==============================================================================
	Module:         SignalPacketBenchmarks
	Description:    Encoding and parsing of the control signals, exactly as
					PeerChannel sends (dump) and routes (parse) them
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <string>

#include "Channel/SignalPacket.h"
#include "PeerValidation/Checks/ICompatibilityCheck.h"

using namespace netlink;
using json = nlohmann::json;


namespace ChannelBenchmarks
{

static SignalPacket makeSignal(const SignalType type)
{
	SignalPacket packet;
	packet.signalType = type;
	packet.senderName = "DESKTOP-4F2K9Q1";

	switch (type)
	{
	case SignalType::ConnectAnswer: packet.payload = PayloadConnectAnswer{.accepted = false, .reason = "Already in a connection"}; break;
	case SignalType::ReadyFlag: packet.payload = PayloadReadyFlag{.ready = true}; break;
	case SignalType::ValidationRequest: packet.payload = PayloadValidationRequest{.request = static_cast<uint8_t>(RemoteRequest::Version)}; break;
	case SignalType::SecretResponse: packet.payload = PayloadSecretResponse{.secret = "a-shared-application-secret"}; break;
	case SignalType::VersionResponse: packet.payload = PayloadVersionResponse{.version = "1.4.2.1337"}; break;
	default: break;
	}

	return packet;
}


static const char *nameOf(const SignalType type)
{
	switch (type)
	{
	case SignalType::ConnectRequest: return "ConnectRequest";
	case SignalType::ConnectAnswer: return "ConnectAnswer";
	case SignalType::Disconnect: return "Disconnect";
	case SignalType::ReadyFlag: return "ReadyFlag";
	case SignalType::ValidationRequest: return "ValidationRequest";
	case SignalType::SecretResponse: return "SecretResponse";
	case SignalType::VersionResponse: return "VersionResponse";
	case SignalType::ValidationHandshake: return "ValidationHandshake";
	}
	return "?";
}


static void signalTypes(benchmark::internal::Benchmark *b)
{
	b->ArgName("signal");
	for (int64_t type = 0; type <= static_cast<int64_t>(SignalType::ValidationHandshake); ++type)
		b->Arg(type);
}


static void BM_SignalPacket_Encode(benchmark::State &state)
{
	const auto type	  = static_cast<SignalType>(state.range(0));
	const auto packet = makeSignal(type);

	for (auto _ : state)
	{
		std::string encoded = json(packet).dump();
		benchmark::DoNotOptimize(encoded);
	}

	state.SetLabel(nameOf(type));
}
BENCHMARK(BM_SignalPacket_Encode)->Apply(signalTypes);


static void BM_SignalPacket_Parse(benchmark::State &state)
{
	const auto		  type	  = static_cast<SignalType>(state.range(0));
	const std::string encoded = json(makeSignal(type)).dump();

	for (auto _ : state)
	{
		auto packet = json::parse(encoded.begin(), encoded.end()).get<SignalPacket>();
		benchmark::DoNotOptimize(packet);
	}

	state.SetLabel(nameOf(type));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * encoded.size()));
}
BENCHMARK(BM_SignalPacket_Parse)->Apply(signalTypes);

} // namespace ChannelBenchmarks
