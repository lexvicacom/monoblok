// libFuzzer harness for the patchbay json rule-file loader: parse, resolve,
// validate, and evaluate. See fuzz_patchbay_common.h.
#include "fuzz_patchbay_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return fuzz_pb_run("fuzz.json", data, size);
}
