#ifndef ASMPRINTER_H
#define ASMPRINTER_H

#include "rv/MCFunction.h"
#include "IR/Module.h"
#include <ostream>
#include <vector>
#include <memory>

namespace sysy {
namespace rv {

class AsmPrinter {
public:
    void run(const std::vector<std::unique_ptr<MCFunction>>& funcs,
            Module* module, std::ostream& os);

private:
    void emitText(const std::vector<std::unique_ptr<MCFunction>>& funcs,
                std::ostream& os);
    void emitGlobals(Module* module, std::ostream& os);
};

} // namespace rv
} // namespace sysy

#endif
