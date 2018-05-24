#define _DEFAULT_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <asm-generic/errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <assert.h>
#include <X11/Xlib.h>



//#define _BSD_SOURCE

FILE *fp;

int WIDTH  = 256;
int HEIGHT = 32;

// -1 means center of the screen
int X = -1;
int Y = -1;

int SECONDS_DELAY = 2;


char ICON_TEXT[32] = "%";


// Defines
#define CPU_FILE_PATH "/proc/stat"
#define CPU_MAX_VALS 10
#define SEC_TO_WAIT 2
#define WAIT_UNTIL_UPDATE 25000 //ns

// CPU Calculation
typedef struct CpuInfo CpuInfo;
struct CpuInfo {
  uint64_t idle;
  uint64_t total;
  uint32_t perc;
};

typedef struct CpuCalcRefreshInfo CpuCalcRefreshInfo;
struct CpuCalcRefreshInfo {
  pthread_t thread;            // Thread that calculates cpu percent
  pthread_mutex_t wait_mutex;  // Interval wait mutex
  pthread_cond_t wait_cond;    // Interval wait condition variable
  CpuInfo *cpu_info;
  size_t num_cpus;
};

//GLOBALS
static CpuCalcRefreshInfo cpu_calc_refresh_info_;
static bool cpu_calc_stop_refresh_cond_ = false;

// The delay between refreshing cpu values (ns)
unsigned long REFRESH_SPEED = 50000;

void get_cpu_usage_from_core(float *cpu, int core);

size_t get_num_cpus(const char *file);

// Note: Returns true if it has timed out or false when the condition variable has been notified
static bool cond_timedwait(time_t seconds, bool *cond_stop, pthread_mutex_t *mutex, pthread_cond_t *cond_var) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += seconds;
  bool timedout = false;
  pthread_mutex_lock(mutex);
  while (!*cond_stop)
    if (ETIMEDOUT == pthread_cond_timedwait(cond_var, mutex, &deadline)) {
      timedout = true;
      break;
    }
  pthread_mutex_unlock(mutex);
  return timedout;
}

// CPU Calculation (Thread 1)
static bool cpu_calc_refresh_timedwait(time_t seconds) {
  return cond_timedwait(seconds, &cpu_calc_stop_refresh_cond_, &cpu_calc_refresh_info_.wait_mutex, &cpu_calc_refresh_info_.wait_cond);
}

static void get_perc_info(CpuInfo *cpu_info, uint64_t *cpu_vals, uint64_t prev_idle, uint64_t prev_total) {
  assert(cpu_info);
  assert(cpu_vals);
  cpu_info->idle = cpu_vals[ 3 ];
  cpu_info->total = 0L;
  for (size_t i = 0U; i < CPU_MAX_VALS; ++i)
    cpu_info->total += cpu_vals[ i ];
  const uint64_t diff_idle = cpu_info->idle - prev_idle;
  const uint64_t diff_total = cpu_info->total - prev_total;
  cpu_info->perc = (100 * (diff_total - diff_idle)) / diff_total;
}

static void refresh_cpu_calc(const char *file, size_t ncpus) {
  assert(file);
  uint64_t cpus_file_info[ ncpus ][ CPU_MAX_VALS ];
  uint64_t prev_idle[ ncpus ], prev_total[ ncpus ];
  memset(prev_idle, 0, sizeof(prev_idle));
  memset(prev_total, 0, sizeof(prev_total));

  while (true) {
    // Open the file
    FILE *const fd = fopen(file, "r");
    if (!fd)
      return;

    // Do the percent calculation
    char buf[ 256 ];
    for (size_t i = 0U; i < ncpus; ++i) {
      fgets(buf, sizeof(buf), fd);
      if (EOF == sscanf(buf + 5, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
          " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
          cpus_file_info[ i ] + 0, cpus_file_info[ i ] + 1,
          cpus_file_info[ i ] + 2, cpus_file_info[ i ] + 3,
          cpus_file_info[ i ] + 4, cpus_file_info[ i ] + 5,
          cpus_file_info[ i ] + 6, cpus_file_info[ i ] + 7,
          cpus_file_info[ i ] + 8, cpus_file_info[ i ] + 9))
        return;
      get_perc_info(cpu_calc_refresh_info_.cpu_info + i, cpus_file_info[ i ], prev_idle[ i ], prev_total[ i ]);
      prev_idle[ i ] = cpu_calc_refresh_info_.cpu_info[ i ].idle;
      prev_total[ i ] = cpu_calc_refresh_info_.cpu_info[ i ].total;
    }

    // Close the file
    fclose(fd);

    // Wait 1 second or break if the conditional variable has been signaled
    if (!cpu_calc_refresh_timedwait(SEC_TO_WAIT))
      break;
  }
}

static void *refresh_cpu_calc_thread(void *args) {
  (void)args;
  refresh_cpu_calc(CPU_FILE_PATH, cpu_calc_refresh_info_.num_cpus);
  pthread_exit(NULL);
}

static bool init_cpu_calc_thread(void) {
  // Init mutex and cond
  pthread_mutex_init(&cpu_calc_refresh_info_.wait_mutex, NULL);
  pthread_cond_init(&cpu_calc_refresh_info_.wait_cond, NULL);

  // Create thread
  return 0 == pthread_create(&cpu_calc_refresh_info_.thread, NULL, refresh_cpu_calc_thread, NULL);
}

static void stop_cpu_calc_thread(void) {
  // Stop calc thread
  cpu_calc_stop_refresh_cond_ = true;
  pthread_cond_broadcast(&cpu_calc_refresh_info_.wait_cond);

  // Join thread
  void *status;
  if (pthread_join(cpu_calc_refresh_info_.thread, &status))  // Wait
    perror("stop_cpu_calc_thread - Could not join thread");

  // Destroy cond and mutex
  pthread_cond_destroy(&cpu_calc_refresh_info_.wait_cond);
  pthread_mutex_destroy(&cpu_calc_refresh_info_.wait_mutex);
}

static bool init_cpu_calc_refresh_info(void) {
  cpu_calc_refresh_info_.num_cpus = get_num_cpus(CPU_FILE_PATH);
  if (cpu_calc_refresh_info_.num_cpus <= 0)
    return false;
  cpu_calc_refresh_info_.cpu_info = (CpuInfo *)calloc(cpu_calc_refresh_info_.num_cpus, sizeof(CpuInfo));
  return cpu_calc_refresh_info_.cpu_info != NULL;
}

static void stop_cpu_calc_refresh_info(void) {
  free(cpu_calc_refresh_info_.cpu_info);
  cpu_calc_refresh_info_.cpu_info = NULL;
}


size_t get_num_cpus(const char *file) 
{
  FILE *const fd = fopen(file, "r");
  if (!fd)
    return 0;
  size_t i = 0U;
  char buf[ 256 ];
  while (fgets(buf, sizeof(buf), fd)) {
    if (strncmp(buf, "cpu", 3) != 0)
      break;
    ++i;
  }
  fclose(fd);
  return i;
}

int main(int argc, char *argv[])
{

    if (!init_cpu_calc_refresh_info())
      printf("Could not init cpu calc refresh info\n");
    if (!init_cpu_calc_thread())
      printf("Could not init cpu calc thread\n");
            
    // get X and Y
    Display *xdisp = XOpenDisplay(NULL);
    Screen *screen = DefaultScreenOfDisplay(xdisp);

    // screw division, bit shift to the right
    if(X == -1)
        X = (screen->width >> 1) - (WIDTH >> 1);

    if(Y == -1)
        Y = (screen->height >> 1) - HEIGHT;

    // Calculate the progress bar width and height from a base percentage.
    // I calculated these by coming up with a solid foundation for dimensions, then
    // divided to get a percentage for preservation of proportions.
    long pbar_height = lround(HEIGHT * 0.34);
    long pbar_max_width = lround(WIDTH * 0.586);
    long ltext_x = lround(WIDTH * 0.078);
    long rtext_x = lround(WIDTH * 0.78);

    char *command = malloc(sizeof(char) * 256);

    char BG[24];
    char FG[24];
    char FONT[270];

    sprintf(command, "dzen2 -ta l -x %d -y %d -w %d -h %d %s %s %s", X, Y, WIDTH, HEIGHT, BG, FG, FONT);

    FILE *stream = popen(command, "w");

    free(command);

    float prev_cpu = 0;

    // The time we should stop
    time_t current_time = (unsigned)time(NULL);
    time_t stop_time = current_time + SECONDS_DELAY;

    while(current_time <= stop_time)
    {
        current_time = (unsigned)time(NULL);

        float cpu = ((float)cpu_calc_refresh_info_.cpu_info[0].perc / (float)100);
        printf("%f\n", cpu);
        
        if(prev_cpu != cpu)
        {
            char *string = malloc(sizeof(char) * 512);
            sprintf(string, "^pa(+%ldX)%s^pa()  ^r%s(%ldx%ld) ^pa(+%ldX)%3.0f%%^pa()\n",
                    ltext_x, ICON_TEXT, "",  //"o" für innen leer
                    lround(pbar_max_width * cpu), pbar_height, rtext_x, cpu*100);
            
            fprintf(stream, string);
            fflush(stream);
            free(string);

            prev_cpu = cpu;
            stop_time = current_time + SECONDS_DELAY;
        }

        usleep(REFRESH_SPEED);
    }

    fclose(fp);

    fflush(NULL);
    pclose(stream);
    return 0;
}

