#include "libhfuzz/libhfuzz.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "honggfuzz.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfuzz/instrument.h"

__attribute__((used)) const char* LIBHFUZZ_module_persistent = _HF_PERSISTENT_SIG;

__attribute__((weak)) int LLVMFuzzerTestOneInput(const uint8_t* buf, size_t len);
__attribute__((weak)) int LLVMFuzzerInitialize(int* argc, char*** argv);
__attribute__((weak)) size_t LLVMFuzzerMutate(
    uint8_t* Data HF_ATTR_UNUSED, size_t Size HF_ATTR_UNUSED, size_t MaxSize HF_ATTR_UNUSED) {
    LOG_F("LLVMFuzzerMutate() is not supported in honggfuzz yet");
    return 0;
}

static uint8_t buf[_HF_PERF_BITMAP_SIZE_16M] = {0};

void HonggfuzzFetchData(const uint8_t** buf_ptr, size_t* len_ptr) {
    static bool initialized = false;
    if (initialized) {
        if (!files_writeToFd(_HF_PERSISTENT_FD, &HFdoneTag, sizeof(HFdoneTag))) {
            LOG_F("writeToFd(size=%zu, doneTag) failed", sizeof(HFdoneTag));
        }
    } else {
        /*
         * Start coverage feedback from this point only (ignore coverage obtained during process
         * start-up)
         */
        instrumentClearNewCov();
        if (!files_writeToFd(_HF_PERSISTENT_FD, &HFreadyTag, sizeof(HFreadyTag))) {
            LOG_F("writeToFd(size=%zu, readyTag) failed", sizeof(HFreadyTag));
        }
        initialized = true;
    }

    uint64_t rcvLen;
    if (files_readFromFd(_HF_PERSISTENT_FD, (uint8_t*)&rcvLen, sizeof(rcvLen)) != sizeof(rcvLen)) {
        LOG_F("readFromFd(rcvLen, size=%zu) failed", sizeof(rcvLen));
    }

    *buf_ptr = instrumentFileBuf();
    *len_ptr = (size_t)rcvLen;
}

void HF_ITER(const uint8_t** buf_ptr, size_t* len_ptr) {
    return HonggfuzzFetchData(buf_ptr, len_ptr);
}

static void HonggfuzzRunOneInput(const uint8_t* buf, size_t len) {
    int ret = LLVMFuzzerTestOneInput(buf, len);
    if (ret != 0) {
        LOG_F("LLVMFuzzerTestOneInput() returned '%d' instead of '0'", ret);
    }
}

static void HonggfuzzPersistentLoop(void) {
    for (;;) {
        size_t len;
        const uint8_t* buf;

        HonggfuzzFetchData(&buf, &len);
        HonggfuzzRunOneInput(buf, len);
    }
}

static int HonggfuzzRunFromFile(int argc, char** argv) {
    int in_fd = STDIN_FILENO;
    const char* fname = "[STDIN]";
    if (argc > 1) {
        fname = argv[argc - 1];
        if ((in_fd = open(argv[argc - 1], O_RDONLY)) == -1) {
            PLOG_W("Cannot open '%s' as input, using stdin", argv[argc - 1]);
            in_fd = STDIN_FILENO;
            fname = "[STDIN]";
        }
    }

    LOG_I(
        "Accepting input from '%s'\nUsage for fuzzing: honggfuzz -P [flags] -- %s", fname, argv[0]);

    ssize_t len = files_readFromFd(in_fd, buf, sizeof(buf));
    if (len < 0) {
        LOG_E("Couldn't read data from stdin: %s", strerror(errno));
        return -1;
    }

    HonggfuzzRunOneInput(buf, len);
    return 0;
}

int HonggfuzzMain(int argc, char** argv) {
    if (LLVMFuzzerInitialize) {
        LLVMFuzzerInitialize(&argc, &argv);
    }

    if (LLVMFuzzerTestOneInput == NULL) {
        LOG_F(
            "Define 'int LLVMFuzzerTestOneInput(uint8_t * buf, size_t len)' in your "
            "code to make it work");
    }

    if (fcntl(_HF_PERSISTENT_FD, F_GETFD) != -1) {
        HonggfuzzPersistentLoop();
    }

    return HonggfuzzRunFromFile(argc, argv);
}

/*
 * Declare it 'weak', so it can be safely linked with regular binaries which
 * implement their own main()
 */
#if !defined(__CYGWIN__)
__attribute__((weak))
#endif /* !defined(__CYGWIN__) */
int main(int argc, char** argv) {
    return HonggfuzzMain(argc, argv);
}
