#include <ydb/library/actors/dnsresolver/dnsresolver.h>
#include <ydb/library/actors/testlib/test_runtime.h>

using namespace NActors;
using namespace NActors::NDnsResolver;


int main(int argc, char** argv) {
    if (argc != 4) {
        Cout << "Usage: " << argv[0] << " <resolver_type> <add_trailing_dot> <hostname>" << Endl;
        return 1;
    }
    EDnsResolverType resolverType = strcmp(argv[1], "ares") == 0 ? EDnsResolverType::Ares : EDnsResolverType::Libc;
    bool addTrailingDot = strcmp(argv[2], "true") == 0;
    const char* hostname = argv[3];
    Cout << "Using resolver type: " << (resolverType == EDnsResolverType::Ares ? "Ares" : "Libc") << Endl;
    Cout << "Using add trailing dot: " << (addTrailingDot ? "true" : "false") << Endl;
    Cout << "Using hostname: " << hostname << Endl;

    TSimpleDnsResolverOptions options { .Type = resolverType, .AddTrailingDot = addTrailingDot }; 
    TTestActorRuntimeBase runtime;
    runtime.Initialize();
    auto sender = runtime.AllocateEdgeActor();
    auto resolver = runtime.Register(CreateSimpleDnsResolver(options));
    runtime.Send(new IEventHandle(resolver, sender, new TEvDns::TEvGetHostByName(hostname, AF_UNSPEC)),
            0, true);
    auto ev = runtime.GrabEdgeEventRethrow<TEvDns::TEvGetHostByNameResult>(sender);

    for (const auto& addr : ev->Get()->AddrsV4) {
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, str, sizeof(str));
        Cout << "IPv4: " << str << Endl;
    }
    for (const auto& addr : ev->Get()->AddrsV6) {
        char str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, str, sizeof(str));
        Cout << "IPv4: " << str << Endl;
    }
    return 0;
}
