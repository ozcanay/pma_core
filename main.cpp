#include <iostream>
#include <vector>
#include <map>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <x86intrin.h>

static constexpr long CHA_MSR_PMON_CTRL_BASE = 0x0E01L;
static constexpr long CHA_MSR_PMON_CTR_BASE = 0x0E08L;

static constexpr unsigned int LEFT_READ = 0x004003AB; /// horizontal_bl_ring
static constexpr unsigned int RIGHT_READ = 0x00400CAB; /// horizontal_bl_ring
static constexpr unsigned int UP_READ = 0x004003AA; /// vertical_bl_ring
static constexpr unsigned int DOWN_READ = 0x00400CAA; /// vertical_bl_ring
static constexpr unsigned int CORE_PMA = 0x00401017; /// core_pma --> GV bxxx1xxxx GV
static constexpr unsigned int FILTER0 = 0x0000003B; /// FILTER0.NULL
static constexpr unsigned int FILTER1 = 0x0000003B; /// FILTER1.NULL

static constexpr int CACHE_LINE_SIZE = 64;
static constexpr int NUM_SOCKETS = 1;
static constexpr int NUM_CHA_BOXES = 18;
static constexpr int NUM_CHA_COUNTERS = 4;

volatile int val = 0;

using namespace std;

int stick_this_thread_to_core(int core_id) {
        int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (core_id < 0 || core_id >= num_cores)
                return EINVAL;

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);

        pthread_t current_thread = pthread_self();
        return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

int main(int argc, char** argv)
{
    if(argc != 2) {
        cout << "Usage: " << argv[0] << " <LOGICAL CORE NUMBER WE WANT TO BIND THREAD TO>" << endl;
        exit(EXIT_FAILURE);
    }

    int lproc = std::stoi(argv[1]);

    std::string file_name = "core" + std::string(argv[1]);
    freopen(file_name.c_str(), "w", stdout);


    long logical_core_count = sysconf(_SC_NPROCESSORS_ONLN);
    std::vector<int> msr_fds(logical_core_count);
    char filename[100];

    cout << "logical core count: " << logical_core_count << endl;

    for(auto i = 0; i < logical_core_count; ++i) {
        sprintf(filename, "/dev/cpu/%d/msr",i);
        int fd = open(filename, O_RDWR);
        if (msr_fds[i] == -1) {
            std::cout << "could not open." << std::endl;
            exit(-1);
        } else {
            msr_fds[i] = fd;
            cout << "msr_fds[" << i << "] = " << fd << endl;
        }
    }

    uint64_t msr_val = 0;
    uint64_t msr_num = 0;
    ssize_t rc64 = 0;

    std::vector<unsigned int> counters{CORE_PMA};

    for(int socket = 0; socket < NUM_SOCKETS; ++socket) {
        for(int cha = 0; cha < NUM_CHA_BOXES; ++cha) {
            long core = 0; /// modify using the first core.

            for(int counter = 0; counter < counters.size(); ++counter) {
                msr_val = counters[counter];
                msr_num = CHA_MSR_PMON_CTRL_BASE + (0x10 * cha) + counter;
                // msr_fds[0] for socket 0, 
                rc64 = pwrite(msr_fds[core], &msr_val, sizeof(msr_val), msr_num);
                if(rc64 != 8) {
                    fprintf(stdout, "ERROR writing to MSR device on core %d, write %ld bytes\n", core, rc64);
                    exit(EXIT_FAILURE);
                } else {
                    cout << "Configuring socket" << socket << "-CHA" << cha << " by writing 0x" << std::hex << msr_val 
                    << " to core " << std::dec << core << ", offset 0x" << std::hex << msr_num << std::dec << std::endl; 
                }
            }
        }
    }

    //int num_elements = 536870912;
    /// create 2GB of data in RAM that would be accessed by core 0 in the next step.
    //void *vals = nullptr;
    //posix_memalign(&vals, CACHE_LINE_SIZE, num_elements * sizeof(int));
    //int* data = (int*)vals;

    //cout << "Flushing data from cache..." << endl;
    //for(int i = 0; i < num_elements; i = i + CACHE_LINE_SIZE) {
    //    _mm_clflush(&data[i]);
    //}
    //cout << "Flushed data from cache." << endl;

    std::map<pair<int, int>, pair<uint64_t, uint64_t>> socket_cha_before_after_pma_gv_map;

    cout << "Sticking main thread to core " << lproc << endl;
    stick_this_thread_to_core(lproc);

    cout << "---------------- FIRST READINGS ----------------" << endl;
    for(int socket = 0; socket < NUM_SOCKETS; ++socket) {
        long core = 0;

        for(int cha = 0; cha < NUM_CHA_BOXES; ++cha) {
            for(int counter = 0; counter < counters.size(); ++counter) {
                msr_num = CHA_MSR_PMON_CTR_BASE + (0x10*cha) + counter;
                rc64 = pread(msr_fds[core], &msr_val, sizeof(msr_val), msr_num);
                if (rc64 != sizeof(msr_val)) {
                    exit(EXIT_FAILURE);
                } else {
                    socket_cha_before_after_pma_gv_map[make_pair(socket, cha)].first = msr_val;
                    std::cout << "Read " << msr_val << " from socket" << socket << "-CHA" << cha << 
                    " on core " << core << ", offset 0x" << std::hex << msr_num << std::dec << std::endl;
                }
            }
        }
    }

    for(int ii = 0; ii < 1000000000; ++ii){
	++val;
    }

    //for(int i = 0; i < num_elements; ++i) {
    //    data[i] += 5;
    //}

    cout << "---------------- SECOND READINGS ----------------" << endl;
    for(int socket = 0; socket < NUM_SOCKETS; ++socket) {
        long core = 0;

        for(int cha = 0; cha < NUM_CHA_BOXES; ++cha) {
            for(int counter = 0; counter < counters.size(); ++counter) {
                msr_num = CHA_MSR_PMON_CTR_BASE + (0x10*cha) + counter;
                rc64 = pread(msr_fds[core], &msr_val, sizeof(msr_val), msr_num);
                if (rc64 != sizeof(msr_val)) {
                    exit(EXIT_FAILURE);
                } else {
                    socket_cha_before_after_pma_gv_map[make_pair(socket, cha)].second = msr_val;
                    std::cout << "Read " << msr_val << " from socket" << socket << "-CHA" << cha << 
                    " on core " << core << ", offset 0x" << std::hex << msr_num << std::dec << std::endl;
                }
            }
        }
    }
    
    cout << "---------------- PMA GV ANALYSIS ----------------" << endl;
    // cout << "Read data from memory on core: " << lproc << ". Core is supposed to be on socket" << ((lproc < 18) ? 0 : 1) << endl;

    for(int socket = 0; socket < NUM_SOCKETS; ++socket) {
        uint64_t max_val_for_socket = 0;
        int max_cha = -1;
        
        cout << "--- SOCKET" << socket << " ---" << endl; 
        for(int cha = 0; cha < NUM_CHA_BOXES; ++cha) {
            uint64_t diff = socket_cha_before_after_pma_gv_map[{socket, cha}].second - socket_cha_before_after_pma_gv_map[{socket, cha}].first;

            cout << "cha: " << cha << ", PMA GV before: " 
            << socket_cha_before_after_pma_gv_map[{socket, cha}].first
            << ", PMA GV after: " << socket_cha_before_after_pma_gv_map[{socket, cha}].second << "; Diff: " 
            << diff << endl;

            if(diff > max_val_for_socket) {
                max_val_for_socket = diff;
                max_cha = cha;
            }
        }
        cout << "Max PMA GV value " << max_val_for_socket << " for socket" << socket << " observed on CHA " << max_cha << endl;
    }

    //cout << "Flushing data from cache again at the end..." << endl;
    //for(int i = 0; i < num_elements; i = i + CACHE_LINE_SIZE) {
    //    _mm_clflush(&data[i]);
    //}
    //cout << "Flushed data from cache again at the end." << endl;

    return 0;
}
