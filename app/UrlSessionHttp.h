#pragma once

#include "client.h"

namespace NHoshi {

// IHttp over NSURLSession. Send blocks the calling thread, so it must not run on the main thread;
// HSSession calls it from its own serial queue.
class TUrlSessionHttp : public IHttp {
public:
    THttpResponse Send(const THttpRequest& request) override;
};

} // namespace NHoshi
