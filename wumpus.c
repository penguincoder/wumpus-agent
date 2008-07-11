/*
 * Wum+ -- A wumpus clone with a self-solving intelligent agent.
 * Andrew Coleman <mercury at penguincoder dot org>
 * Copyleft 2007 -- Licensed under GNU GPLv2
 * See http://gnu.org/licenses/old-licenses/gpl-2.0.html for more details.
 *
 * CSC-4240 Program 2.
 *
 * This program uses SQLite 3 so many of the database functions use a specific
 * callback that is generally prefixed with the name of the calling function
 * and suffixed with _callback. These are not used anywhere else and should
 * not be used normally.
 *
 * This is an exceedingly complicated program that could easily be an end of
 * semester project or a project to work on throughout the class. This is only
 * the second program due. There were also no real usable Wumpus clones in C.
 * This one had to be written from naught.
 *
 * How to compile:
 * gcc -Os -Wall -lsqlite3 -lm -o wumplus wumpus.c
 *
 * How to use to play the game:
 * ./wumplus
 *
 * How to use to make the agent play:
 * ./wumplus --agent
 *
 */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <math.h>

/* Constants for map elements */
#define MAP_SIZE 14
#define MAP_MAXSTEPS 500
#define MAP_PLAYER '@'
#define MAP_EMPTY '.'
#define MAP_WALL '#'
#define MAP_PIT 'P'
#define MAP_WUMPUS 'W'
#define MAP_GOLD 'G'
#define MAP_SUPMUW 'S'

/* Constants for percepts */
#define PERCEPT_BUMP 1
#define PERCEPT_SMELL 2
#define PERCEPT_BREEZE 4
#define PERCEPT_MOO 8
#define PERCEPT_GLITTER 16
#define PERCEPT_DEAD 32
#define PERCEPT_WUMPUS 64
#define PERCEPT_SUPMUW 128
#define PERCEPT_PIT 256
#define PERCEPT_SAFE 512
#define PERCEPT_VISITED 1024
#define PERCEPT_DESTINATION 2048

/* constants for the direction of the move or arrow */
#define DIRECTION_NORTH 1
#define DIRECTION_EAST 2
#define DIRECTION_SOUTH 3
#define DIRECTION_WEST 4

/* constants for scoring */
#define SCORE_MOVE -1
#define SCORE_DEATH -1000
#define SCORE_SHOOT -10
#define SCORE_KILL 500
#define SCORE_GOLD 1000
#define SCORE_FOOD 100
#define SCORE_MIN -1000

/* queue elements for determining a path to a place, needs x,y in one bucket */
typedef struct COORD {
  int x, y;
} coordinate;

/*
 * struct for managing the whole game
 * i wasn't going to make this global, but somehow passing a pointer to one
 * of these structs around somehow causes the whole thing to get stupidly
 * corrupted and cause segfaults...
 */
struct WUMPLUS {
  /* general settings */
  int x, y, arrows, percepts, score, steps_taken, dest_x, dest_y;
  /* flags */
  short int has_food, has_gold, supmuw_neighbors_wumpus, use_agent;
  /* the map */
  char map[MAP_SIZE][MAP_SIZE];
  /* the knowledge base */
  sqlite3 *db;
} game;

/* map initialization functions */
int random_map_coordinate();
void random_map_x_y(int *, int *);
void init_game();

/* interaction functions */
void process_percepts();
void unknown_action();

/* player inputs */
void process_player_command(char);
void user_input();
void agent_input();

/* game outputs */
void print_help();
void print_map();
void print_percepts();
void print_score();

/* game helpers */
int player_dead();
int has_won();
int has_lost();
char *delta_coordinates(int *, int *, int);

/* game actions */
void add_score(int);
void action_move(int);
void action_shoot(int);
void action_grab();
void action_quit();

/* agent stuff, yeah, there's a lot... */
void kb_init();
void kb_close();
static int kb_found_callback(void *, int, char **, char **);
int kb_found(int, int, int);
int visited(int, int);
int safe(int, int);
int wall(int, int);
int glitter(int, int);
int smell(int, int);
void kb_insert(int, int, int);
void kb_delete(int, int, int);
void check_corner(int, int, int, int);
void kb_inferrances(int, int);
void kb_tell();
void remove_destination();
int has_destination();
int at_destination();
int at_start();
static int huss_callback(void *, int, char **, char **);
int has_unvisited_safe_squares();
char relative_direction(int, int);
char shortest_path();
int wumpus_nearby(coordinate *);
char kb_ask_action();
char *word_from_percept(int);
static int kb_dump_callback(void *, int, char **, char **);
void kb_dump();

/* list / queue functions (SQL based!) */
void queue_make_empty(const char *);
static int queue_empty_callback(void *, int, char **, char **);
int queue_empty(const char *);
void queue_enqueue(const char *, coordinate *);
static int dequeue_callback(void *, int, char **, char **);
void queue_dequeue(const char *, coordinate *);

/*
 * This is the main game loop. Checks for the command line argument and runs
 * the input loop.
 */
int main(int argc, char **argv)
{
  game.use_agent = 0;
  /* check for agent usage */
  if(argc == 2 && strcmp(argv[1], "--agent") == 0)
    game.use_agent = 1;
  
  printf("Wum+ By Andrew Coleman <mercury at penguincoder dot org>\n");
  printf("Scoring:\n");
  printf(" Move (%d), Death (%d), Shoot (%d)\n", SCORE_MOVE, SCORE_DEATH,
    SCORE_SHOOT);
  printf(" Food (%d), Gold (%d), Kill Wumpus(%d)\n", SCORE_FOOD, SCORE_GOLD,
    SCORE_KILL);
  printf("Available Percepts: [Bump,Smell,Breeze,Moo,Glitter,Dead]\n");
  printf("Losing Conditions: Score < %d or Steps > %d or Dead\n",
    SCORE_MIN, MAP_MAXSTEPS);
  printf("Winning Conditions: Gold and Player in starting position (1,1).\n");
  printf("Invocate program with --agent to run as F.O.L. agent\n");
  
  /* initialize game */
  srand(time(NULL));
  init_game();
  
  /* main game loop */
  process_percepts();
  do
  {
    printf("\n");
    /* pretty map it if we are using */
    if(game.use_agent)
      print_map();
    /* show the user */
    print_percepts();
    /* remove this now, otherwise it sticks */
    if(game.percepts & PERCEPT_BUMP)
      game.percepts ^= PERCEPT_BUMP;
    /* show the score */
    print_score();
    /* get the requested action */
    game.use_agent ? agent_input() : user_input();
    /* figure out what's going on */
    process_percepts();
  } while(!has_won() && !has_lost());
  
  /* fin */
  action_quit();
  return 0;
}

/* Returns a valid random coordinate for the map, not including a wall */
int random_map_coordinate()
{
  return (rand() % (MAP_SIZE - 2)) + 1;
}

/* Randomly places the coordinate pair to an empty spot in the map */
void random_map_x_y(int *map_x, int *map_y)
{
  int x = 0, y = 0;
  do {
    x = random_map_coordinate(); y = random_map_coordinate();
  } while((x == 1 && y == 1) || game.map[x][y] != MAP_EMPTY);
  *map_x = x; *map_y = y;
}

/* Intialize map with randomly placed obstackles */
void init_game()
{
  int i, j, num_pits, num_walls, x, y;
  
  /* flag for determining if the supmuw is next to the wumpus. */
  game.supmuw_neighbors_wumpus = 0;
  
  /* First create a Clean Slate */
  for(j = 0; j < MAP_SIZE; j++)
    for(i = 0; i < MAP_SIZE; i++)
      game.map[i][j] = MAP_EMPTY;
  
  /* Place player at (1,1) */
  game.x = 1;
  game.y = 1;
  game.has_food = 0;
  game.has_gold = 0;
  game.arrows = 1;
  game.percepts = 0;
  game.score = 0;
  game.steps_taken = 0;
  game.dest_x = -1;
  game.dest_y = -1;
  
  /* Create walls around perimeter of map. one loop. figure it out. */
  for(i = 0; i < MAP_SIZE; i++)
  {
    game.map[i][0] = MAP_WALL;
    game.map[i][MAP_SIZE - 1] = MAP_WALL;
    game.map[0][i] = MAP_WALL;
    game.map[MAP_SIZE - 1][i] = MAP_WALL;
  }
  
  /* I maximize the number of pits to be 15% the size of the map */
  num_pits = (rand() % (int)(MAP_SIZE * MAP_SIZE * .15)) + 1;
  for(i = 0; i < num_pits; i++)
  {
    random_map_x_y(&x, &y);
    game.map[x][y] = MAP_PIT;
  }
  
  /* set up the interior walls in random locations. max 10% of mapsize */
  num_walls = (rand() % (int)(MAP_SIZE * MAP_SIZE * .10)) + 1;
  for(i = 0; i < num_walls; i++)
  {
    random_map_x_y(&x, &y);
    game.map[x][y] = MAP_WALL;
  }
  
  /* Create Wumpus at Random Location */
  random_map_x_y(&x, &y);
  game.map[x][y] = MAP_WUMPUS;
  
  /* Randomly place a pot - o - gold */
  random_map_x_y(&x, &y);
  game.map[x][y] = MAP_GOLD;
  
  /* Place the Supmuw (wumpus cousin) */
  random_map_x_y(&x, &y);
  game.map[x][y] = MAP_SUPMUW;
  /* check to see if the supmuw neighbors the wumpus, used for percepts */
  if(game.map[x][y + 1] == MAP_WUMPUS ||
     game.map[x][y - 1] == MAP_WUMPUS ||
     game.map[x + 1][y] == MAP_WUMPUS ||
     game.map[x - 1][y] == MAP_WUMPUS)
  {
    game.supmuw_neighbors_wumpus = 1;
  }
  
  /* set up the database for the KB */
  if(game.use_agent)
  {
    kb_init();
    /* let the kb know about the outside walls. */
    for(i = 0; i < MAP_SIZE; i++)
    {
      kb_insert(PERCEPT_BUMP, i, 0);
      kb_insert(PERCEPT_BUMP, i, MAP_SIZE - 1);
      kb_insert(PERCEPT_BUMP, 0, i);
      kb_insert(PERCEPT_BUMP, MAP_SIZE - 1, i);
    }
  }
}

/*
 * Processes the player position and determines if any percepts fire.
 * This checks the map given the player's current position and sets all flags
 * as appropriate. The only flag not set is the PERCEPT_BUMP flag which _must_
 * be set by the action_move() function since the player cannot occupy the
 * same square as the wall.
 */
void process_percepts()
{
  int x = game.x, y = game.y, flags = 0;
  char north = game.map[x][y - 1], south = game.map[x][y + 1];
  char east = game.map[x + 1][y], west = game.map[x - 1][y];
  
  /* the move function sets this percept */
  int bumped = game.percepts & PERCEPT_BUMP;
  if(bumped)
    flags |= PERCEPT_BUMP;
  /* see if the player is dead, first */
  if(game.map[x][y] == MAP_PIT || game.map[x][y] == MAP_WUMPUS ||
     (game.map[x][y] == MAP_SUPMUW && game.supmuw_neighbors_wumpus))
  {
    flags |= PERCEPT_DEAD;
    add_score(SCORE_DEATH);
    if(game.map[x][y] == MAP_PIT)
      printf("You have fallen into a pit!\n");
    else
      printf("You have been consumed by the beast!\n");
  }
  if(north == MAP_WUMPUS ||
     south == MAP_WUMPUS ||
     east == MAP_WUMPUS ||
     west == MAP_WUMPUS)
    flags |= PERCEPT_SMELL;
  if(north == MAP_PIT ||
     south == MAP_PIT ||
     east == MAP_PIT ||
     west == MAP_PIT)
    flags |= PERCEPT_BREEZE;
  if(north == MAP_SUPMUW ||
     south == MAP_SUPMUW ||
     east == MAP_SUPMUW ||
     west == MAP_SUPMUW)
    flags |= PERCEPT_MOO;
  if(game.map[x][y] == MAP_GOLD)
    flags |= PERCEPT_GLITTER;
  if(flags & PERCEPT_MOO && game.supmuw_neighbors_wumpus)
    flags |= PERCEPT_SMELL;
  game.percepts = flags;
  if(game.use_agent)
    kb_tell();
}

/* unknown action */
void unknown_action()
{
  printf("Do what now? (Unknown action)\n");
}

/* does what the player wants */
void process_player_command(char choice)
{
  switch(choice)
  {
    case '?':
      print_help();
      break;
    case 'q':
      action_quit();
      break;
    case 'n':
    case 'k':
      action_move(DIRECTION_NORTH);
      break;
    case 's':
    case 'j':
      action_move(DIRECTION_SOUTH);
      break;
    case 'e':
    case 'l':
      action_move(DIRECTION_EAST);
      break;
    case 'w':
    case 'h':
      action_move(DIRECTION_WEST);
      break;
    case 'N':
      action_shoot(DIRECTION_NORTH);
      break;
    case 'S':
      action_shoot(DIRECTION_SOUTH);
      break;
    case 'E':
      action_shoot(DIRECTION_EAST);
      break;
    case 'W':
      action_shoot(DIRECTION_WEST);
      break;
    case 'g':
      action_grab();
      break;
    default:
      unknown_action();
  }
}

/* get user defined inputs */
void user_input()
{
  char choice;
  printf("Enter a Command (?): ");
  scanf("%1s", &choice);
  process_player_command(choice);
}

/*
 * get agent (AI) desired action, asks the knowledge base and guesses for the
 * best course of action.
 */
void agent_input()
{
  char choice = kb_ask_action();
  printf("agent_input: %c\n", choice);
  process_player_command(choice);
}

/* prints help for a user */
void print_help()
{
  printf("Usable commands:\n");
  printf(" n,s,e,w    Move in direction given (also VI keybindings)\n");
  printf(" N,S,E,W    Shoot in direction given\n");
  printf(" g          Grab gold\n");
  printf(" q          Quit\n");
}

/* display the current environment */
void print_map()
{
  int i = 0, j = 0;
  for(j = 0; j < MAP_SIZE; j++)
  {
    for(i = 0; i < MAP_SIZE; i++)
    {
      if(i == game.x && j == game.y)
        printf("%c", MAP_PLAYER);
      else
        printf("%c", game.map[i][j]);
    }
    printf("\n");
  }
  printf("\n");
}

/* prints out what percepts the player feels */
void print_percepts()
{
  char *nopercept = "None";
  printf("Percepts: [");
  printf("%s,", (game.percepts & PERCEPT_BUMP ? "Bump" : nopercept));
  printf("%s,", (game.percepts & PERCEPT_SMELL ? "Smell" : nopercept));
  printf("%s,", (game.percepts & PERCEPT_BREEZE ? "Breeze" : nopercept));
  printf("%s,", (game.percepts & PERCEPT_MOO ? "Moo" : nopercept));
  printf("%s,", (game.percepts & PERCEPT_GLITTER ? "Glitter" : nopercept));
  printf("%s", (game.percepts & PERCEPT_DEAD ? "Dead" : nopercept));
  printf("]\n");
}

/* prints out the player's score */
void print_score()
{
  printf("Score: %5d\tSteps Taken: %3d/%d\n", game.score, game.steps_taken,
    MAP_MAXSTEPS);
}

/* helper to tell if the player is dead */
int player_dead()
{
  return (game.percepts & PERCEPT_DEAD);
}

/* you have won when you have the gold and are at the start square */
int has_won()
{
  return (game.x == 1 && game.y == 1 && game.has_gold);
}

/*
 * you lose when your score is less than SCORE_MIN or have taken more than
 * MAP_MAXSTEPS or you have died.
 */
int has_lost()
{
  return (game.score < SCORE_MIN || game.steps_taken > MAP_MAXSTEPS ||
    player_dead());
}

/*
 * figures out which direction the user wants to go and updates
 * pointers to the new coordinates. returns a string for printing which
 * direction was processed.
 */
char *delta_coordinates(int *x, int *y, int direction)
{
  /* add the direction onto the coordinate */
  switch(direction)
  {
    case DIRECTION_NORTH:
      (*y)--;
      return "North";
      break;
    case DIRECTION_SOUTH:
      (*y)++;
      return "South";
      break;
    case DIRECTION_EAST:
      (*x)++;
      return "East";
      break;
    case DIRECTION_WEST:
      (*x)--;
      return "West";
      break;
  }
  return "Unknown";
}

/* adds score into the game */
void add_score(int delta)
{
  game.score += delta;
}

/* moves the player around. also requires a direction. */
void action_move(int direction)
{
  int x2 = game.x, y2 = game.y;
  add_score(SCORE_MOVE);
  game.steps_taken++;
  printf("Moving %s ", delta_coordinates(&x2, &y2, direction));
  printf("(%d, %d)\n", x2, y2);
  
  /* this function will process bumps */
  if(game.map[x2][y2] == MAP_WALL)
  {
    game.percepts |= PERCEPT_BUMP;
    printf("You bumped into a wall!\n");
    /* just go ahead and back out if you bump into something */
    if(game.use_agent)
    {
      /* must tell the kb about this... */
      kb_insert(PERCEPT_BUMP, x2, y2);
    }
    return;
  }
  
  /* see if you are in the same square as a supmuw */
  if(game.map[x2][y2] == MAP_SUPMUW &&
     !game.has_food && !game.supmuw_neighbors_wumpus)
  {
    game.has_food = 1;
    printf("The supmuw has gifted food to you!\n");
    add_score(SCORE_FOOD);
  }
  
  /* now go ahead and move the player */
  game.x = x2; game.y = y2;
}

/* shoots arrows. requires a direction */
void action_shoot(int direction)
{
  int x2 = game.x, y2 = game.y;

  if(!game.arrows)
  {
    printf("You are out of arrows!\n");
    return;
  }
  
  printf("Shooting %s\n", delta_coordinates(&x2, &y2, direction));
  add_score(SCORE_SHOOT);
  game.arrows--;
  if(game.map[x2][y2] == MAP_WUMPUS || game.map[x2][y2] == MAP_SUPMUW)
  {
    add_score(SCORE_KILL);
    printf("You hear a deafening scream as you slay the beast.\n");
    game.map[x2][y2] = MAP_EMPTY;
    /* regardless of who you kill, the supmuw does not neighbor wumpus */
    game.supmuw_neighbors_wumpus = 0;

    /* tell the agent that the thing was killed */    
    if(game.use_agent)
    {
      /* only one of these will be removed */
      kb_delete(PERCEPT_WUMPUS, x2, y2);
      kb_delete(PERCEPT_SUPMUW, x2, y2);
      /* remove the smells, too */
      kb_delete(PERCEPT_SMELL, x2 - 1, y2);
      kb_delete(PERCEPT_SMELL, x2 + 1, y2);
      kb_delete(PERCEPT_SMELL, x2, y2 - 1);
      kb_delete(PERCEPT_SMELL, x2, y2 + 1);
    }
  }
}

/* grabs gold if possible */
void action_grab()
{
  if(game.map[game.x][game.y] == MAP_GOLD)
  {
    add_score(SCORE_GOLD);
    printf("You have found gold!\n");
    game.map[game.x][game.y] = MAP_EMPTY;
    game.has_gold = 1;
    if(game.use_agent)
      kb_delete(PERCEPT_GLITTER, game.x, game.y);
  }
}

/* quits the game and returns to the OS */
void action_quit()
{
  printf("\nFinal Analysis of gameplay\n");
  /* show the final map */
  print_map();
  /* show the final set of percepts */
  print_percepts();
  
  /* one last chance to make fun of the player */
  if(has_lost())
    printf("Apparently you are not a winner. That would make you a loser.\n");
  if(player_dead())
    printf("You have died. Indiana Jones would be ashamed.\n");
  if(has_won())
    printf("You have won, the plantation is saved. Glory! Glory!\n");
  
  /* final score */
  print_score();
  
  /* cleanup for agent */
  if(game.use_agent)
  {
    /* dumps the contents of the knowledge base to stderr */
    kb_dump();
    kb_close();
  }
  exit(0);
}

/* initialize the knowledge base. builds an sqlite3 RAM db and build tables */
void kb_init()
{
  char *err_msg;
  int res = 0;
  
  /* make db */
  res = sqlite3_open(":memory:", &game.db);
  if(res)
  {
    fprintf(stderr, "KB_INIT: %s\n",
      sqlite3_errmsg(game.db));
    sqlite3_close(game.db);
    exit(1);
  }
  
  /* create the knowledge base table */
  res = sqlite3_exec(game.db,
    /* no primary key, you get locking errors if you do... */
    "CREATE TABLE kb (sentence INT, x INT, y INT);",
    NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_INIT: %s\n", err_msg);
    sqlite3_free(err_msg);
    exit(1);
  }
  
  /* create the queue table */
  res = sqlite3_exec(game.db,
    "CREATE TABLE queue (id INTEGER PRIMARY KEY, name VARCHAR, x INT, y INT);",
    NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_INIT: %s\n", err_msg);
    sqlite3_free(err_msg);
    exit(1);
  }
}

/* closes the database stuff */
void kb_close()
{
  sqlite3_close(game.db);
}

/* private callback that just sees if a row has been found */
static int kb_found_callback(void *found, int argc, char **argv, char **cols)
{
  if(argc > 0)
    *((int *)found) = 1;
  return 0;
}

/* finds a row in the kb */
int kb_found(int sentence, int x, int y)
{
  int res = 0, found = 0;
  char query[128], *err_msg;
  
  sprintf(query,
    "SELECT * FROM KB WHERE x = %d AND y = %d AND sentence = %d LIMIT 1;",
    x, y, sentence);
  res = sqlite3_exec(game.db, query, kb_found_callback, (void *)&found,
    &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_FOUND: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
  return found;
}

/* has the square been visited? */
int visited(int x, int y)
{
  return kb_found(PERCEPT_VISITED, x, y);
}

/* is the square safe? */
int safe(int x, int y)
{
  return kb_found(PERCEPT_SAFE, x, y);
}

/* is it a wall? */
int wall(int x, int y)
{
  return kb_found(PERCEPT_BUMP, x, y);
}

/* does it glitter? */
int glitter(int x, int y)
{
  return kb_found(PERCEPT_GLITTER, x, y);
}

/* does it smell? */
int smell(int x, int y)
{
  return kb_found(PERCEPT_SMELL, x, y);
}

/*
 * puts a percept or sentence into the kb. requires a percept and does not
 * insert a row if one is already found of the same kind and position.
 */
void kb_insert(int sentence, int x, int y)
{
  int res = 0;
  char query[128], *err_msg;
  
  /* don't insert another row of the same kind or for a wall */
  if(kb_found(sentence, x, y))
    return;
  
  sprintf(query, "INSERT INTO KB (sentence, x, y) VALUES (%d, %d, %d);",
    sentence, x, y);
  res = sqlite3_exec(game.db, query, NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_INSERT: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}

/* removes a statement from the database */
void kb_delete(int sentence, int x, int y)
{
  int res = 0;
  char query[128], *err_msg;
  if(!kb_found(sentence, x, y))
    return;
  
  sprintf(query, "DELETE FROM KB WHERE sentence = %d AND x = %d AND y = %d;",
    sentence, x, y);
  res = sqlite3_exec(game.db, query, NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_DELETE: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}

/*
 * generalization for checking around a spot. this will insert something
 * into the KB if found. it takes what it's looking for, what to insert when
 * found and a delta for x and y from the current player position.
 *
 * This requires two pieces of information, one known to be safe and one known
 * to have the percept. If you have more information, then something is not
 * right. If you have less, not enough can be inferred.
 */
void check_corner(int percept, int known, int xd, int yd)
{
  if(kb_found(percept, game.x + xd, game.y + yd) &&
     !wall(game.x + xd, game.y + yd) &&
     safe(game.x + xd, game.y) ^ safe(game.x, game.y + yd))
  {
    if(safe(game.x + xd, game.y))
      kb_insert(known, game.x, game.y + yd);
    if(safe(game.x, game.y + yd))
      kb_insert(known, game.x + xd, game.y);
  }
}

/*
 * check for something at each corner around where the player sits. This only
 * checks the diagonals around the player position. the general form to check
 * the squares immediately accessible to the player require three pieces of
 * information and only in the rarest of cases can you not determine this
 * otherwise. A specific situation is like this:
 *
 * ..P.
 * PPP.
 *
 * The agent does not seem to notice the middle P in the bottom row, but really
 * the agent will not maneuver to that square anyways. Not a big deal, really.
 *
 * This function requires the 'maybe' percept and the percept to tell the KB
 * that you 'found' the obstackle in question.
 */
void kb_inferrances(int percept, int known)
{
  if(!kb_found(percept, game.x, game.y))
    return;
  
  /* check north west corner above player */
  check_corner(percept, known, -1, -1);
  
  /* check north east corner above player */
  check_corner(percept, known, 1, -1);
  
  /* check south west corner below player */
  check_corner(percept, known, -1, 1);
  
  /* check south east corner below player */
  check_corner(percept, known, 1, 1);
}

/*
 * tell the knowledge base about your percepts. this is a lot like processing
 * the percepts, only now we are telling the kb about what we see using only
 * the percepts.
 */
void kb_tell()
{
  kb_insert(PERCEPT_VISITED, game.x, game.y);
  if(!(game.percepts & PERCEPT_DEAD))
    kb_insert(PERCEPT_SAFE, game.x, game.y);
  if(game.percepts & PERCEPT_SMELL)
    kb_insert(PERCEPT_SMELL, game.x, game.y);
  if(game.percepts & PERCEPT_BREEZE)
    kb_insert(PERCEPT_BREEZE, game.x, game.y);
  if(game.percepts & PERCEPT_MOO)
    kb_insert(PERCEPT_MOO, game.x, game.y);
  if(game.percepts & PERCEPT_GLITTER)
    kb_insert(PERCEPT_GLITTER, game.x, game.y);
  if(!(game.percepts & PERCEPT_SMELL) &&
     !(game.percepts & PERCEPT_BREEZE))
  {
    /*
     * this is useful to expand the number of squares we can access after each
     * move.
     */
    kb_insert(PERCEPT_SAFE, game.x - 1, game.y);
    kb_insert(PERCEPT_SAFE, game.x + 1, game.y);
    kb_insert(PERCEPT_SAFE, game.x, game.y - 1);
    kb_insert(PERCEPT_SAFE, game.x, game.y + 1);
  }
  
  /*
   * now lets make some inferrances.
   * this process is mostly the same, so just use the same function.
   */
  kb_inferrances(PERCEPT_SMELL, PERCEPT_WUMPUS);
  kb_inferrances(PERCEPT_BREEZE, PERCEPT_PIT);
  kb_inferrances(PERCEPT_MOO, PERCEPT_SUPMUW);
}

/* removes the pre-set destination, if it exists */
void remove_destination()
{
  kb_delete(PERCEPT_DESTINATION, 0, 0);
  game.dest_x = -1; game.dest_y = -1;
}

/*
 * sets a bee-line destination for the agent to navigate to this point.
 * I use destinations to force the agent to explore more of the map. It is the
 * sai-yu-sanji-kuyah (top priority) next to gold. You set a destination and the
 * agent goes there. Because i always require a coordinate in the KB for each
 * percept saved, i use (0, 0) as the coordinate in the database and keep the
 * real coordinates in the WUMPLUS struct. This simplifies many things about
 * the SQL code and prevents me from having to write another function just to
 * pull the two coordinates out.
 */
void set_destination(int x, int y)
{
  kb_insert(PERCEPT_DESTINATION, 0, 0);
  game.dest_x = x;
  game.dest_y = y;
}

/* does a destination exist? */
int has_destination()
{
  if(kb_found(PERCEPT_DESTINATION, 0, 0))
    return 1;
  return 0;
}

/* is the agent is at the destination? */
int at_destination()
{
  if(has_destination() && game.x == game.dest_x && game.y == game.dest_y)
    return 1;
  return 0;
}

/* is the agent is at the starting position? */
int at_start()
{
  return game.x == 1 && game.y == 1;
}

/* callback to see if an unvisited safe square has been found */
static int huss_callback(void *found, int argc, char **argv, char **cols)
{
  int x = atoi(argv[1]), y = atoi(argv[2]);
  if(!*((int *)found) && !visited(x, y) && !wall(x, y))
  {
    *((int *)found) = 1;
    set_destination(x, y);
  }
  return 0;
}

/* finds a random unvisited safe square and sets the destination thusly */
int has_unvisited_safe_squares()
{
  int res = 0, found = 0;
  char query[128], *err_msg;
  
  sprintf(query, "SELECT * FROM kb WHERE sentence = %d ORDER BY random();",
    PERCEPT_SAFE);
  res = sqlite3_exec(game.db, query, huss_callback, &found, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "HAS_UNVISITED_SAFE_SQUARES: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
  return found;
}

/* returns a direction to the requested square from the relative player pos. */
char relative_direction(int x, int y)
{
  if(x == game.x)
    return (game.y < y ? 's' : 'n');
  if(y == game.y)
    return (game.x < x ? 'e' : 'w');
  return 'q';
}

/*
 * determines if two squares are neighbors.
 * returns a boolean if the delta between the two coordinates is one.
 */
int neighbors(int x1, int y1, int x2, int y2)
{
  int xd = abs(x2 - x1), yd = abs(y2 - y1);
  if(xd > 1 || yd > 1 || xd + yd > 1 || xd + xd == 0)
    return 0;
  return 1;
}

/*
 * will return a char for the next step to get to the given square
 *
 * This function performs a breadth-first search to find the requested square.
 * It also uses a modified A* approach to this. I use the idea of cost-to-next
 * square-plus-heuristic to figure out where to go next. I use the breadth-first
 * traversal and calculate the cost to the destination from that square. It
 * adds one to the weights as it goes out. When you reach the player's square
 * you only have to choose the smallest non-zero square to determine the best
 * direction of travel.
 *
 * This is a lot of complication for a lot of simplification on the game side
 * This is probably the hardest part of the whole program. 'Knowing' things
 * about what you see is easy, inferring this is pretty easy. Getting there.
 * That's complication.
 *
 * This algorithm is also completely capable of navigating 'walls' of obstackles
 * in the game. Pits, walls, wumpuses, supmuws, whatever. It can get around it.
 *
 * ==General procedure==
 * Unmark and set weights to 0 for all squares.
 * Set weight to 1 for destination square.
 * Enqueue the destination.
 * While queue is not empty:
 *  Dequeue first item
 *  Skip if wall, not safe or marked
 *  Mark item
 *  Enqueue all four sides
 *  Set weights on all four sides to one plus current weight,
 *   if weight is zero or larger than desired weight
 * Dump queue
 * Zero all weights for walls or unsafe/unvisited squares
 * Find smallest weight neighboring the player's position
 * Go there.
 */
char shortest_path()
{
  int i = 0, j = 0, marked[MAP_SIZE][MAP_SIZE], weights[MAP_SIZE][MAP_SIZE];
  coordinate possibles[MAP_SIZE][MAP_SIZE], temp, min;
  int new_weight = 0;
  char *queue = "queue";
  
  for(i = 0; i < MAP_SIZE; i++)
  {
    for(j = 0; j < MAP_SIZE; j++)
    {
      marked[i][j] = 0;
      weights[i][j] = 0;
      possibles[i][j].x = i;
      possibles[i][j].y = j;
    }
  }
  
  temp.x = -1;
  temp.y = -1;
  min.x = -1;
  min.y = -1;
  weights[game.dest_x][game.dest_y] = 1;
  queue_enqueue(queue, &possibles[game.dest_x][game.dest_y]);
  
  while(!queue_empty(queue))
  {
    queue_dequeue(queue, &temp);
    if(wall(temp.x, temp.y) || !safe(temp.x, temp.y) || marked[temp.x][temp.y])
      continue;
    
    marked[temp.x][temp.y] = 1;
    queue_enqueue(queue, &possibles[temp.x - 1][temp.y]);
    queue_enqueue(queue, &possibles[temp.x + 1][temp.y]);
    queue_enqueue(queue, &possibles[temp.x][temp.y - 1]);
    queue_enqueue(queue, &possibles[temp.x][temp.y + 1]);
    
    new_weight = weights[temp.x][temp.y] + 1;
    if(weights[temp.x - 1][temp.y] == 0 ||
       weights[temp.x - 1][temp.y] > new_weight)
    {
      weights[temp.x - 1][temp.y] = new_weight;
    }
    if(weights[temp.x + 1][temp.y] == 0 ||
       weights[temp.x + 1][temp.y] > new_weight)
    {
      weights[temp.x + 1][temp.y] = new_weight;
    }
    if(weights[temp.x][temp.y - 1] == 0 ||
       weights[temp.x][temp.y - 1] > new_weight)
    {
      weights[temp.x][temp.y - 1] = new_weight;
    }
    if(weights[temp.x][temp.y + 1] == 0 ||
       weights[temp.x][temp.y + 1] > new_weight)
    {
      weights[temp.x][temp.y + 1] = new_weight;
    }
  }
  queue_make_empty(queue);
  
  for(i = 0; i < MAP_SIZE; i++)
    for(j = 0; j < MAP_SIZE; j++)
      if(wall(j, i) || (!safe(j, i) && !visited(j, i)))
        weights[j][i] = 0;
  
  new_weight = 0;
  temp.x = game.x; temp.y = game.y;
  if((weights[game.x - 1][game.y] < new_weight || new_weight == 0) &&
     weights[game.x - 1][game.y])
  {
    new_weight = weights[game.x - 1][game.y];
    temp.x = game.x - 1;
    temp.y = game.y;
  }
  if((weights[game.x + 1][game.y] < new_weight || new_weight == 0) &&
     weights[game.x + 1][game.y])
  {
    new_weight = weights[game.x + 1][game.y];
    temp.x = game.x + 1;
    temp.y = game.y;
  }
  if((weights[game.x][game.y - 1] < new_weight || new_weight == 0) &&
     weights[game.x][game.y - 1])
  {
    new_weight = weights[game.x][game.y - 1];
    temp.x = game.x;
    temp.y = game.y - 1;
  }
  if((weights[game.x][game.y + 1] < new_weight || new_weight == 0) &&
     weights[game.x][game.y + 1])
  {
    new_weight = weights[game.x][game.y + 1];
    temp.x = game.x;
    temp.y = game.y + 1;
  }
  
  return relative_direction(temp.x, temp.y);
}

/* determines if the (perceived) wumpus is in a nearby square */
int wumpus_nearby(coordinate *wumpus)
{
  int found = 0;
  if(kb_found(PERCEPT_WUMPUS, game.x - 1, game.y))
  {
    wumpus->x = game.x - 1;
    wumpus->y = game.y;
    found = 1;
  }
  if(kb_found(PERCEPT_WUMPUS, game.x + 1, game.y))
  {
    wumpus->x = game.x + 1;
    wumpus->y = game.y;
    found = 1;
  }
  if(kb_found(PERCEPT_WUMPUS, game.x, game.y - 1))
  {
    wumpus->x = game.x;
    wumpus->y = game.y - 1;
    found = 1;
  }
  if(kb_found(PERCEPT_WUMPUS, game.x, game.y + 1))
  {
    wumpus->x = game.x;
    wumpus->y = game.y + 1;
    found = 1;
  }
  return found;
}

/*
 * gets the action from the knowledge base.
 *
 * priority of actions is as follows:
 * 1. grab if glittering and set destination to start
 * 2. kill that wumpus, if you know where he is and he is nearby.
 * 3. go to the destination, if set
 * 4. find an unvisited safe square and go there
 * 5. go back to the beginning and give up.
 *
 * The supmuw will usually be de-food-ified through the normal course of travel
 * so specific rules are not really necessary. Hunting the wumpus is viewed
 * as a 'bonus' more than a goal, so it only happens if the agent discovers
 * where the wumpus is located and then travels to a nearby square.
 */
char kb_ask_action()
{
  coordinate wumpus;
  wumpus.x = game.x; wumpus.y = game.y;
  
  /* priority one: gold */
  if(glitter(game.x, game.y))
  {
    /* if gold, stop, drop and proceed to exit */
    remove_destination();
    set_destination(1, 1);
    return 'g';
  }
  
  /* check to see if the destination is deadly or a wall, if so, remove it */
  if(has_destination() && (wall(game.dest_x, game.dest_y) ||
     !safe(game.dest_x, game.dest_y)))
  {
    remove_destination();
  }
  
  /* kill the wumpus, if he is nearby */
  if(smell(game.x, game.y) && game.arrows && wumpus_nearby(&wumpus))
  {
    return (char)((int)relative_direction(wumpus.x, wumpus.y) - 32);
  }
  
  /*
   * if has destination, go there now. if we're not at the destination, then
   * go ahead and find a new random location and go there if we have unvisited
   * safe areas.
   */
  if((has_destination() && !at_destination()) || has_unvisited_safe_squares())
  {
    return shortest_path();
  }
  
  /*
   * should really hunt the wumpus and find the supmuw now, but we're gonna
   * save that for later... since the objective is only to find the gold, this
   * part is not strictly speaking necessary. Just go back and quit...
   */
  if(!at_start())
  {
    set_destination(1, 1);
    return shortest_path();
  }
  
  /*
   * failure condition, just quit. can't find the gold and can't guarantee
   * that the agent will survive any new squares.
   */
  return 'q';
}

/* returns a word for a percept */
char *word_from_percept(int percept)
{
  char *res = "Unknown";
  switch(percept)
  {
    case(PERCEPT_BUMP):
      res = "BUMP";
      break;
    case(PERCEPT_SMELL):
      res = "SMELL";
      break;
    case(PERCEPT_BREEZE):
      res = "BREEZE";
      break;
    case(PERCEPT_MOO):
      res = "MOO";
      break;
    case(PERCEPT_GLITTER):
      res = "GLITTER";
      break;
    case(PERCEPT_DEAD):
      res = "DEAD";
      break;
    case(PERCEPT_WUMPUS):
      res = "WUMPUS";
      break;
    case(PERCEPT_SUPMUW):
      res = "SUPMUW";
      break;
    case(PERCEPT_PIT):
      res = "PIT";
      break;
    case(PERCEPT_SAFE):
      res = "SAFE";
      break;
    case(PERCEPT_VISITED):
      res = "VISITED";
      break;
    case(PERCEPT_DESTINATION):
      res = "DESTINATION";
      break;
  }
  return res;
}

/* private callback for printing out each row of the knowledge base */
static int kb_dump_callback(void *x, int argc, char **argv, char **cols)
{
  fprintf(stderr, "%4d: %7s: (%2d, %2d)\n", *((int *)x),
    (argv[0] ? word_from_percept(atoi(argv[0])) : "NULL"),
    atoi(argv[1]), atoi(argv[2]));
  *((int *)x) = *((int *)x) + 1;
  return 0;
}

/* dumps the kb's contents to stderr; sorts on value then on column then row */
void kb_dump()
{
  int res = 0, counter = 1;
  char *err_msg;
  
  fprintf(stderr, "Knowledge Base Dump\n");
  res = sqlite3_exec(game.db, "SELECT * FROM kb ORDER BY sentence, y, x;",
    kb_dump_callback, (void *)&counter, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "KB_DUMP: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}

/* empties a queue */
void queue_make_empty(const char *mylist)
{
  int res = 0;
  char query[128], *err_msg;
  sprintf(query, "DELETE FROM queue WHERE name = '%s';", mylist);
  res = sqlite3_exec(game.db, query, NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "QUEUE_MAKE_EMPTY: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}

/* private callback to check if the number of items in a queue is empty */
static int queue_empty_callback(void *empty, int argc, char **argv, char **cols)
{
  int result = atoi(argv[0]);
  if(result > 0)
    *((int *)empty) = 0;
  return 0;
}

/* is the queue empty */
int queue_empty(const char *mylist)
{
  int res = 0, empty = 1;
  char query[128], *err_msg;
  sprintf(query, "SELECT COUNT(*) FROM queue WHERE name = '%s';", mylist);
  res = sqlite3_exec(game.db, query, queue_empty_callback, &empty, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "QUEUE_EMPTY: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
  return empty;
}

/* adds a coordinate into the queue */
void queue_enqueue(const char *mylist, coordinate *data)
{
  int res = 0;
  char query[128], *err_msg;
  
  sprintf(query, "INSERT INTO queue (name, x, y) VALUES ('%s', %d, %d);",
    mylist, data->x, data->y);
  res = sqlite3_exec(game.db, query, NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "QUEUE_ENQUEUE: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}

/* private callback for dequeue to return the coordinate */
static int dequeue_callback(void *coord, int argc, char **argv, char **cols)
{
  coordinate *mycoord = (coordinate *)coord;
  mycoord->x = atoi(argv[2]);
  mycoord->y = atoi(argv[3]);
  return 0;
}

/* removes an item from the queue and returns the values into *result */
void queue_dequeue(const char *mylist, coordinate *result)
{
  int res = 0;
  char query[128], *err_msg;
  
  sprintf(query,
    "SELECT * FROM queue WHERE name = '%s' ORDER BY id ASC LIMIT 1;",
    mylist);
  res = sqlite3_exec(game.db, query, dequeue_callback, result, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "QUEUE_DEQUEUE: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
  
  sprintf(query,
    "DELETE FROM queue WHERE name = '%s' AND x = %d AND y = %d;",
    mylist, result->x, result->y);
  res = sqlite3_exec(game.db, query, NULL, 0, &err_msg);
  if(res != SQLITE_OK)
  {
    fprintf(stderr, "QUEUE_DEQUEUE: %s\n", err_msg);
    sqlite3_free(err_msg);
  }
}
