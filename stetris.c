#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <dirent.h>

// The game state can be used to detect what happens on the playfield
#define GAMEOVER 0
#define ACTIVE (1 << 0)
#define ROW_CLEAR (1 << 1)
#define TILE_ADDED (1 << 2)

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct
{
    bool occupied;
    uint16_t color;
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

// MYCODE BEGINS HERE
#define FBIOGET_FSCREENINFO 0x4602

struct fb_fix_screeninfo {
      char id[16];                    /* identification string eg "TT Builtin" */
      unsigned long smem_start;       /* Start of frame buffer mem */
                                      /* (physical address) */
      __u32 smem_len;                 /* Length of frame buffer mem */
      __u32 type;                     /* see FB_TYPE_*                */
      __u32 type_aux;                 /* Interleave for interleaved Planes */
      __u32 visual;                   /* see FB_VISUAL_*              */
      __u16 xpanstep;                 /* zero if no hardware panning  */
      __u16 ypanstep;                 /* zero if no hardware panning  */
      __u16 ywrapstep;                /* zero if no hardware ywrap    */
      __u32 line_length;              /* length of a line in bytes    */
      unsigned long mmio_start;       /* Start of Memory Mapped I/O   */
                                      /* (physical address) */
      __u32 mmio_len;                 /* Length of Memory Mapped I/O  */
      __u32 accel;                    /* Indicate to driver which     */
                                      /*  specific chip/card we have  */
      __u16 capabilities;             /* see FB_CAP_*                 */
      __u16 reserved[2];              /* Reserved for future compatibility */
};

struct fb_fix_screeninfo fix_info;


// System kolorów dla tiles
#define MAX_COLORS 8
static uint16_t color_palette[MAX_COLORS] = {
    0xF800, // Czerwony
    0x07E0, // Zielony  
    0x001F, // Niebieski
    0xF81F, // Różowy
    0xFFE0, // Żółty
    0x07FF, // Cyjan
    0xFC00, // Pomarańczowy
    0x8410  // Szary
};


//Potrzebne deklaracje
static inline bool tileOccupied(coord const target);

// Mapa kolorów dla każdej pozycji na planszy
static uint16_t tile_colors[8][8] = {0};

// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true


static int fb_fd = -1;          // Deskryptor pliku framebuffera
static uint16_t *fb_map = NULL; // Wskaźnik do mapowanej pamięci
static int joy_fd = -1;         // deskryptor joysticka

bool initializeSenseHat()
{
    ///
    ///  INICJALIZACJA WYŚWIETLACZA
    ///


    // 1. OTWÓRZ URZĄDZENIE FRAMEBUFFER
    // "/dev/fb0" to ścieżka do pierwszego framebuffera w systemie
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd == -1) {
        perror("Nie mogę otworzyć framebuffera");
        return false;
    }
    // fb_fd to "file descriptor" - numer który identyfikuje otwarte urządzenie
    
    // 2. POBRAZ INFORMACJE O FRAMEBUFFERZE  
    
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix_info) == -1) {
        perror("Błąd odczytu informacji o framebufferze");
        close(fb_fd);
        return false;
    }
    // ioctl to funkcja do "kontroli" urządzeń - pobiera informacje
    
    // 3. SPRAWDŹ CZY TO SENSE HAT
    printf("ID framebuffera: %s\n", fix_info.id);
    printf("Rozmiar pamięci: %u bajtów\n", fix_info.smem_len);
    
    // 4. MAPUJ PAMIĘĆ DO PROGRAMU
    // mmap tworzy "bezpośredni dostęp" do pamięci urządzenia
    fb_map = mmap(NULL, fix_info.smem_len, PROT_READ | PROT_WRITE, 
                  MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) {
        perror("Błąd mapowania pamięci");
        close(fb_fd);
        return false;
    }
    // fb_map to wskaźnik do obszaru pamięci gdzie możemy pisać kolory pikseli
    
    // 5. WYCZYŚĆ EKRAN (ustaw wszystkie piksele na czarny)
    memset(fb_map, 0, fix_info.smem_len);
    
    // 6. NARYSUJ JEDEN CZERWONY PIKSEL dla testu
    // Piksel (0,0) - lewy górny róg
    fb_map[10] = 0xF800;  // Czerwony w RGB565


    ///
    ///  INICJALIZACJA JOYSTICKA
    ///

    // INICJALIZACJA JOYSTICKA - SPRAWDŹ 8 EVENTÓW Z WERYFIKACJĄ
    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        perror("Cannot open /dev/input");
        return true; // Pozwól grać z klawiatury
    }
    
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        // Szukaj tylko plików event*
        if (strncmp(entry->d_name, "event", 5) == 0) {
            char event_path[64];
            snprintf(event_path, sizeof(event_path), "/dev/input/%s", entry->d_name);
            
            joy_fd = open(event_path, O_RDONLY | O_NONBLOCK);
            if (joy_fd != -1) {
                char name[256] = "Unknown";
                if (ioctl(joy_fd, EVIOCGNAME(sizeof(name)), name) != -1) {
                    if (strstr(name, "Sense HAT Joystick") != NULL) {
                        printf("Found Sense HAT joystick: %s at %s\n", name, event_path);
                        closedir(dir);
                        return true;
                    }
                }
                close(joy_fd);
                joy_fd = -1;
            }
        }
    }
    closedir(dir);
    
    if (joy_fd == -1) {
        fprintf(stderr, "Could not find Sense HAT joystick\n");
        // Nie zwracaj false - pozwól grać z klawiatury
    }


    
    return true;
}

void draw_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= 8 || y < 0 || y >= 8) return;
    if (fb_map == NULL) return;
    
    // Matryca 8x8, indeksowanie wierszowe
    fb_map[y * 8 + x] = color;
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat()
{
    if (fb_map != NULL && fb_map != MAP_FAILED) {
        munmap(fb_map, fix_info.smem_len);
        fb_map = NULL;
    }
    if (fb_fd != -1) {
        close(fb_fd);
        fb_fd = -1;
    }
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick()
{
    return 0;
}

// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged)
{
    if (!playfieldChanged) return;
    
    for (unsigned int y = 0; y < game.grid.y; y++) {
        for (unsigned int x = 0; x < game.grid.x; x++) {
            coord pos = {x, y};
            tile* current_tile = &game.playfield[y][x];
            
            if (tileOccupied(pos)) {
                // Jeśli tile nie ma jeszcze koloru, przypisz losowy
                if (current_tile->color == 0) {
                    current_tile->color = color_palette[rand() % MAX_COLORS];
                }
                draw_pixel(x, y, current_tile->color);
            } else {
                // Tile jest pusty - wyczyść
                current_tile->color = 0;
                draw_pixel(x, y, 0x0000);
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

    //Init random
    srand(time(NULL));


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
            key = readKeyboard();
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
