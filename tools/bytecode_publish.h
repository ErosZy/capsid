#ifndef CAPSID_TOOLS_BYTECODE_PUBLISH_H
#define CAPSID_TOOLS_BYTECODE_PUBLISH_H
#include <cerrno>

namespace capsid {
namespace tools {
struct PublishResult {
    bool complete;
    bool created_final;
    int error;
    const char* operation;
};

inline bool unsupported_no_replace(int error) {
    if (error == EINVAL || error == ENOSYS) return true;
#ifdef EOPNOTSUPP
    if (error == EOPNOTSUPP) return true;
#endif
#ifdef ENOTSUP
    if (error == ENOTSUP) return true;
#endif
    return false;
}

// Each operation returns 0 on success or its immediately captured errno.
// A link publishes a new final name atomically without replacing an existing
// name. Keep ownership when removing the temporary link fails: the caller
// must roll back that final too, rather than misclassifying it as foreign.
template<class Rename, class Link, class RemoveTemporary>
PublishResult publish_no_replace(Rename rename, Link link, RemoveTemporary remove_temporary) {
    int error = rename();
    if (!error) return {true, true, 0, "renameat2"};
    if (!unsupported_no_replace(error)) return {false, false, error, "renameat2"};
    error = link();
    if (error) return {false, false, error, "linkat"};
    error = remove_temporary();
    return {!error, true, error, "unlink temporary"};
}
}  // namespace tools
}  // namespace capsid
#endif
