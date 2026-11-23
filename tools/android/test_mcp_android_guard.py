"""Check the __ANDROID__ guard added to McpClient.cpp's StdioClient constructor.

Two things can be wrong with a guard like this, and both are checkable without an Android
toolchain:
  1. the Linux path must still compile exactly as before (it is what every desktop build uses);
  2. the Android path must contain nothing that needs Bionic -- since the whole point is that
     posix_spawn does not exist there, one leftover statement means the link still fails.
So the Linux half is compiled for the host, and the Android half is asserted to be only the two
casts and a throw.
"""
import subprocess

SOURCE = 'editor/cli/McpClient.cpp'

src = open(SOURCE, encoding='utf-8').read()
posix_start = src.index('\n#else\n\nStdioClient::StdioClient(')
ctor_start = src.index('StdioClient::StdioClient(const fs::path& executable', posix_start)
ctor_end = src.index('bool StdioClient::exited(int& exitCode) {', ctor_start)
ctor = src[ctor_start:ctor_end]

guard = ctor.index('#ifdef __ANDROID__')
middle = ctor.index('#else', guard)


def matching_endif(text, start):
    """The #endif that closes the #ifdef at `start`, counting the nesting inside it (the Linux
    branch has an #ifdef __linux__ of its own)."""
    depth = 0
    for offset, line in enumerate(text[start:].splitlines()):
        stripped = line.strip()
        if stripped.startswith(('#ifdef', '#ifndef', '#if ')):
            depth += 1
        elif stripped.startswith('#endif'):
            depth -= 1
            if depth == 0:
                return start + sum(len(l) + 1 for l in text[start:].splitlines()[:offset])
    raise SystemExit('no matching #endif in McpClient.cpp')


close = matching_endif(ctor, guard)

android_branch = ctor[guard + len('#ifdef __ANDROID__'):middle]
linux_branch = ctor[middle + len('#else'):close]

# 1. the Linux half, compiled for the host
program = '''
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <string>
#include <vector>
namespace fs = std::filesystem;
enum Exit { Connection };
struct ClientError { ClientError(Exit, const std::string&) {} };
struct StdioClient {
    std::string stderrLog; long childPid = -1; intptr_t toChild = -1, fromChild = -1;
    StdioClient(const fs::path&, const std::vector<std::string>&, const fs::path&);
};
extern char** environ;
StdioClient::StdioClient(const fs::path& executable, const std::vector<std::string>& args, const fs::path& workdir)
    : stderrLog(workdir / "editor.stderr.log") {
''' + linux_branch + '''
}
int main() { return 0; }
'''
open('/tmp/mcp_linux.cpp', 'w', encoding='utf-8').write(program)
result = subprocess.run(['g++', '-std=c++20', '-fsyntax-only', '/tmp/mcp_linux.cpp'],
                        capture_output=True, text=True)
linux_ok = result.returncode == 0
print('linux path still compiles:', 'yes' if linux_ok else 'NO')
if not linux_ok:
    print(result.stderr[:3000])

# 2. the Android half: only casts and a throw
statements = []
for line in android_branch.splitlines():
    text = line.strip()
    if not text or text.startswith('//'):
        continue
    statements.append(text)
android_ok = all(
    s.startswith('(void)') or s.startswith('throw') or s.startswith('"')
    for s in statements)
print('android path statements:', statements)
print('android path needs nothing from Bionic:', 'yes' if android_ok else 'NO')

raise SystemExit(0 if (linux_ok and android_ok) else 1)
