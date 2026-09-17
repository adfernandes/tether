// Seeds and reads a nonempty encrypted journal across container recreation.
// Test-image only: no synthetic phone data or test binaries ship in the runtime.
#include <ctime>
#include <string_view>
#include <sys/stat.h>
#include <tether/bluetooth/journal.hpp>

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    umask(0077);
    tether::secret::set_retention(tether::Retention::Encrypted);
    tether::bluetooth::MessageJournal journal;
    if (!journal.open())
        return 1;
    if (std::string_view(argv[1]) == "seed") {
        tether::bluetooth::Message message;
        message.handle = "container-fixture";
        message.thread_key = "tel:+15555550100";
        message.body = "synthetic retained message";
        message.timestamp = std::time(nullptr);
        journal.append(message);
        journal.close();
        return 0;
    }
    if (std::string_view(argv[1]) != "check")
        return 2;
    const auto messages = journal.load(std::time(nullptr));
    return messages.size() == 1 && messages[0].handle == "container-fixture" &&
                   messages[0].body == "synthetic retained message"
               ? 0
               : 1;
}
