#pragma once

namespace framegen {

class GlRenderDiagnostics {
public:
    static void initialize() noexcept;
    static void shutdown() noexcept;
};

} // namespace framegen
