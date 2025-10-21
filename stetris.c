#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <errno.h>
// The game state can be used to detect what happens on the playfield
#define GAMEOVER 0
#define ACTIVE (1 << 0)
#define ROW_CLEAR (1 << 1)
#define TILE_ADDED (1 << 2)

int fb_fd;
int js_fd;
u_int16_t *fbmem;
struct fb_fix_screeninfo fix;
struct fb_var_screeninfo var;
int color = 0; //to alternate colors

#define WHITEHEX 0xFFFF
#define BLUEHEX  0x001F
#define GREENHEX 0x07E0
#define REDHEX   0xF800

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct
{
    bool occupied;
    u_int16_t color;
} tile;

typedef struct
{
    unsigned int x;
    unsigned int y;
} coord;

typedef struct
{
    coord const grid;                     // playfield bounds
    unsigned long const uSecTickTime;     // tick rate
    unsigned long const rowsPerLevel;     // speed up after clearing rows
    unsigned long const initNextGameTick; // initial value of nextGameTick

    unsigned int tiles; // number of tiles played
    unsigned int rows;  // number of rows cleared
    unsigned int score; // game score
    unsigned int level; // game level

    tile *rawPlayfield; // pointer to raw memory of the playfield
    tile **playfield;   // This is the play field array
    unsigned int state;
    coord activeTile; // current tile

    unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                                // when reached 0, next game state calculated
    unsigned long nextGameTick; // sets when tick is wrapping back to zero
                                // lowers with increasing level, never reaches 0
} gameConfig;

gameConfig game = {
    .grid = {8, 8},
    .uSecTickTime = 10000,
    .rowsPerLevel = 2,
    .initNextGameTick = 50,
};


#define TARGET_FB_NAME "RPi-Sense FB" 

int open_framebuffer_by_name(const char *target_name) {
    struct dirent *entry;
    DIR *dp = opendir("/sys/class/graphics");
    if (!dp) {
        perror("opendir");
        return -1;
    }

    while ((entry = readdir(dp)) != NULL) {
        if (strncmp(entry->d_name, "fb", 2) != 0)
            continue; // Skip non-fb entries

        // Build path to name file
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/graphics/%s/name", entry->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char fb_name[128];
        if (fgets(fb_name, sizeof(fb_name), f)) {
            // Remove trailing newline
            fb_name[strcspn(fb_name, "\n")] = 0;

            if (strcmp(fb_name, target_name) == 0) {
                // Found matching framebuffer
                fclose(f);
                closedir(dp);

                // Build /dev/fbN path
                char devpath[128];
                snprintf(devpath, sizeof(devpath), "/dev/%s", entry->d_name);
                int fd = open(devpath, O_RDWR);
                if (fd == -1) {
                    perror("open framebuffer");
                    return -1;
                }
                return fd;
            }
        }
        fclose(f);
    }

    closedir(dp);
    fprintf(stderr, "Framebuffer \"%s\" not found\n", target_name);
    return -1;
}

#define JOYSTICK "Raspberry Pi Sense HAT Joystick"

int get_joystick_fd(const char *target_name) {
    char filename[64];
    char name[256];
    int fd = -1;
    int i = 0;

    while (1) {
        snprintf(filename, sizeof(filename), "/dev/input/event%d", i);
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT)
                break;  // no more devices
            else {
                i++;
                continue;  // skip inaccessible device (permission error, etc.)
            }
        }

        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            close(fd);
            i++;
            continue;
        }

        if (strcmp(name, target_name) == 0) {
            // Found it — set non-blocking mode
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            return fd;
        }

        close(fd);
        i++;
    }

    return -1;
}

// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
bool initializeSenseHat()
{    
    // --- setup of screen ---
    //get file descriptor of framebuffer
    fb_fd = open_framebuffer_by_name(TARGET_FB_NAME);
    if (fb_fd == -1) return false;
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);
    fbmem = (u_int16_t *)mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
        if (fbmem == MAP_FAILED) return false;
    // --- joystick setup ---
    js_fd = get_joystick_fd(JOYSTICK);
    if(!js_fd) {
    return false;
  }
    return true;
}
void renderSenseHatMatrix(bool const playfieldChanged);
static inline void resetPlayfield();
// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat()
{
  //black out field before quitting
  resetPlayfield();
  renderSenseHatMatrix(true);
  //close file descriptor and unmap memory
  if (close(fb_fd) == -1) {
  perror("close");
  }
  munmap(&fbmem, fix.smem_len);
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
enum {
    JS_UP = 0,
    JS_DOWN,
    JS_LEFT,
    JS_RIGHT,
    JS_ENTER,
    JS_COUNT
};

int js_state[JS_COUNT] = {0};
unsigned long last_tick[JS_COUNT] = {0};

// cooldowns per direction (in ticks)
#define COOLDOWN_UP     0
#define COOLDOWN_DOWN   49
#define COOLDOWN_LEFT   25
#define COOLDOWN_RIGHT  25
#define COOLDOWN_ENTER  0

static const int cooldown_ticks[JS_COUNT] = {
    COOLDOWN_UP,
    COOLDOWN_DOWN,
    COOLDOWN_LEFT,
    COOLDOWN_RIGHT,
    COOLDOWN_ENTER
};

int readSenseHatJoystick()
{
    struct input_event event;
    struct pollfd pfd = { .fd = js_fd, .events = POLLIN };

    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        while (1) {
            ssize_t n = read(pfd.fd, &event, sizeof(event));
            if (n == sizeof(event)) {
                if (event.type == EV_KEY) {
                    switch (event.code) {
                        case KEY_UP:    js_state[JS_UP]    = event.value; break;
                        case KEY_DOWN:  js_state[JS_DOWN]  = event.value; break;
                        case KEY_LEFT:  js_state[JS_LEFT]  = event.value; break;
                        case KEY_RIGHT: js_state[JS_RIGHT] = event.value; break;
                        case KEY_ENTER: js_state[JS_ENTER] = event.value; break;
                        default: break;
                    }

                    // Reset cooldown when released
                    if (event.value == 0) {
                        switch (event.code) {
                            case KEY_UP:    last_tick[JS_UP]    = 0; break;
                            case KEY_DOWN:  last_tick[JS_DOWN]  = 0; break;
                            case KEY_LEFT:  last_tick[JS_LEFT]  = 0; break;
                            case KEY_RIGHT: last_tick[JS_RIGHT] = 0; break;
                            case KEY_ENTER: last_tick[JS_ENTER] = 0; break;
                            default: break;
                        }
                    }
                }
            } else {
                if (n < 0 && errno == EAGAIN)
                    break;
                else
                    perror("read");
            }
        }
    }

    for (int i = 0; i < JS_COUNT; i++) {
        if (js_state[i]) {
            unsigned long dt = (game.tick >= last_tick[i])
                ? (game.tick - last_tick[i])
                : (game.nextGameTick + game.tick - last_tick[i]); // handle wrap-around

            if (last_tick[i] == 0 || dt >= cooldown_ticks[i]) {
                last_tick[i] = game.tick;
                switch (i) {
                    case JS_UP:    return KEY_UP;
                    case JS_DOWN:  return KEY_DOWN;
                    case JS_LEFT:  return KEY_LEFT;
                    case JS_RIGHT: return KEY_RIGHT;
                    case JS_ENTER: return KEY_ENTER;
                }
            }
        }
    }

    return 0;
}
// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged)
{
    (void)playfieldChanged;
    if(playfieldChanged) {
    for(int i = 0; i < game.grid.y; i++) {
      for(int j= 0; j < game.grid.x; j++) {
        if(game.playfield[i][j].occupied == true) {
          fbmem[i*game.grid.x + j] = game.playfield[i][j].color;
        } else {
          fbmem[i*game.grid.x + j] = 0x0000;  
        }
      }
    }
  }
}

// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target)
{
    game.playfield[target.y][target.x].occupied = true;
    int16_t tilecolor;
    switch (color) {
    case 0: tilecolor = BLUEHEX ; break;
    case 1: tilecolor = REDHEX  ; break;
    case 2: tilecolor = GREENHEX; break;
    case 3: tilecolor = WHITEHEX; break;
    }
    game.playfield[target.y][target.x].color = tilecolor;
    color = (color+1)%4;
}

static inline void copyTile(coord const to, coord const from)
{
    memcpy((void *)&game.playfield[to.y][to.x], (void *)&game.playfield[from.y][from.x], sizeof(tile));
}

static inline void copyRow(unsigned int const to, unsigned int const from)
{
    memcpy((void *)&game.playfield[to][0], (void *)&game.playfield[from][0], sizeof(tile) * game.grid.x);
}

static inline void resetTile(coord const target)
{
    memset((void *)&game.playfield[target.y][target.x], 0, sizeof(tile));
}

static inline void resetRow(unsigned int const target)
{
    memset((void *)&game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

static inline bool tileOccupied(coord const target)
{
    return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target)
{
    for (unsigned int x = 0; x < game.grid.x; x++)
    {
        coord const checkTile = {x, target};
        if (!tileOccupied(checkTile))
        {
            return false;
        }
    }
    return true;
}

static inline void resetPlayfield()
{
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        resetRow(y);
    }
}

// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile()
{
    game.activeTile.y = 0;
    game.activeTile.x = (game.grid.x - 1) / 2;
    if (tileOccupied(game.activeTile))
        return false;
    newTile(game.activeTile);
    return true;
}

bool moveRight()
{
    coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
    if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool moveLeft()
{
    coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
    if (game.activeTile.x > 0 && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool moveDown()
{
    coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
    if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile))
    {
        copyTile(newTile, game.activeTile);
        resetTile(game.activeTile);
        game.activeTile = newTile;
        return true;
    }
    return false;
}

bool clearRow()
{
    if (rowOccupied(game.grid.y - 1))
    {
        for (unsigned int y = game.grid.y - 1; y > 0; y--)
        {
            copyRow(y, y - 1);
        }
        resetRow(0);
        return true;
    }
    return false;
}

void advanceLevel()
{
    game.level++;
    switch (game.nextGameTick)
    {
    case 1:
        break;
    case 2 ... 10:
        game.nextGameTick--;
        break;
    case 11 ... 20:
        game.nextGameTick -= 2;
        break;
    default:
        game.nextGameTick -= 10;
    }
}

void newGame()
{
    game.state = ACTIVE;
    game.tiles = 0;
    game.rows = 0;
    game.score = 0;
    game.tick = 0;
    game.level = 0;
    resetPlayfield();
}

void gameOver()
{
    game.state = GAMEOVER;
    game.nextGameTick = game.initNextGameTick;
}

bool sTetris(int const key)
{
    bool playfieldChanged = false;

    if (game.state & ACTIVE)
    {
        // Move the current tile
        if (key)
        {
            playfieldChanged = true;
            switch (key)
            {
            case KEY_LEFT:
                moveLeft();
                break;
            case KEY_RIGHT:
                moveRight();
                break;
            case KEY_DOWN:
                while (moveDown())
                {
                };
                game.tick = 0;
                break;
            default:
                playfieldChanged = false;
            }
        }

        // If we have reached a tick to update the game
        if (game.tick == 0)
        {
            // We communicate the row clear and tile add over the game state
            // clear these bits if they were set before
            game.state &= ~(ROW_CLEAR | TILE_ADDED);

            playfieldChanged = true;
            // Clear row if possible
            if (clearRow())
            {
                game.state |= ROW_CLEAR;
                game.rows++;
                game.score += game.level + 1;
                if ((game.rows % game.rowsPerLevel) == 0)
                {
                    advanceLevel();
                }
            }

            // if there is no current tile or we cannot move it down,
            // add a new one. If not possible, game over.
            if (!tileOccupied(game.activeTile) || !moveDown())
            {
                if (addNewTile())
                {
                    game.state |= TILE_ADDED;
                    game.tiles++;
                }
                else
                {
                    gameOver();
                }
            }
        }
    }

    // Press any key to start a new game
    if ((game.state == GAMEOVER) && key)
    {
        playfieldChanged = true;
        newGame();
        addNewTile();
        game.state |= TILE_ADDED;
        game.tiles++;
    }

    return playfieldChanged;
}

int readKeyboard()
{
    struct pollfd pollStdin = {
        .fd = STDIN_FILENO,
        .events = POLLIN};
    int lkey = 0;

    if (poll(&pollStdin, 1, 0))
    {
        lkey = fgetc(stdin);
        if (lkey != 27)
            goto exit;
        lkey = fgetc(stdin);
        if (lkey != 91)
            goto exit;
        lkey = fgetc(stdin);
    }
exit:
    switch (lkey)
    {
    case 10:
        return KEY_ENTER;
    case 65:
        return KEY_UP;
    case 66:
        return KEY_DOWN;
    case 67:
        return KEY_RIGHT;
    case 68:
        return KEY_LEFT;
    }
    return 0;
}

void renderConsole(bool const playfieldChanged)
{
    if (!playfieldChanged)
        return;

    // Goto beginning of console
    fprintf(stdout, "\033[%d;%dH", 0, 0);
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fprintf(stdout, "\n");
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        fprintf(stdout, "|");
        for (unsigned int x = 0; x < game.grid.x; x++)
        {
            coord const checkTile = {x, y};
            fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
        }
        switch (y)
        {
        case 0:
            fprintf(stdout, "| Tiles: %10u\n", game.tiles);
            break;
        case 1:
            fprintf(stdout, "| Rows:  %10u\n", game.rows);
            break;
        case 2:
            fprintf(stdout, "| Score: %10u\n", game.score);
            break;
        case 4:
            fprintf(stdout, "| Level: %10u\n", game.level);
            break;
        case 7:
            fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
            break;
        default:
            fprintf(stdout, "|\n");
        }
    }
    for (unsigned int x = 0; x < game.grid.x + 2; x++)
    {
        fprintf(stdout, "-");
    }
    fflush(stdout);
}

inline unsigned long uSecFromTimespec(struct timespec const ts)
{
    return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // This sets the stdin in a special state where each
    // keyboard press is directly flushed to the stdin and additionally
    // not outputted to the stdout
    {
        struct termios ttystate;
        tcgetattr(STDIN_FILENO, &ttystate);
        ttystate.c_lflag &= ~(ICANON | ECHO);
        ttystate.c_cc[VMIN] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
    }

    // Allocate the playing field structure
    game.rawPlayfield = (tile *)malloc(game.grid.x * game.grid.y * sizeof(tile));
    game.playfield = (tile **)malloc(game.grid.y * sizeof(tile *));
    if (!game.playfield || !game.rawPlayfield)
    {
        fprintf(stderr, "ERROR: could not allocate playfield\n");
        return 1;
    }
    for (unsigned int y = 0; y < game.grid.y; y++)
    {
        game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
    }

    // Reset playfield to make it empty
    resetPlayfield();
    // Start with gameOver
    gameOver();

    if (!initializeSenseHat())
    {
        fprintf(stderr, "ERROR: could not initilize sense hat\n");
        return 1;
    };

    // Clear console, render first time
    fprintf(stdout, "\033[H\033[J");
    renderConsole(true);
    renderSenseHatMatrix(true);

    while (true)
    {
        struct timeval sTv, eTv;
        gettimeofday(&sTv, NULL);

        int key = readSenseHatJoystick();
        if (!key)
        {
            // NOTE: Uncomment the next line if you want to test your implementation with
            // reading the inputs from stdin. However, we expect you to read the inputs directly
            // from the input device and not from stdin (you should implement the readSenseHatJoystick
            // method).
            //key = readKeyboard();
        }
        if (key == KEY_ENTER)
            break;

        bool playfieldChanged = sTetris(key);
        renderConsole(playfieldChanged);
        renderSenseHatMatrix(playfieldChanged);

        // Wait for next tick
        gettimeofday(&eTv, NULL);
        unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
        if (uSecProcessTime < game.uSecTickTime)
        {
            usleep(game.uSecTickTime - uSecProcessTime);
        }
        game.tick = (game.tick + 1) % game.nextGameTick;
    }

    freeSenseHat();
    free(game.playfield);
    free(game.rawPlayfield);

    return 0;
}
