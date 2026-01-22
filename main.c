#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define NUM_TRADERS 3
#define BUFFER_SIZE 10
#define INITIAL_BALANCE 10000.0

// --- STRUKTÚRÁK ---

// 1. Tranzakció (Láncolt lista elem)
typedef struct Transaction {
    char type[5];       // "BUY" vagy "SELL"
    char stock[10];     // Pl. "AAPL"
    int quantity;
    double price;
    struct Transaction* next; // Mutató a következő elemre
} Transaction;

// 2. Részvény árfolyam (Buffer elem)
typedef struct {
    char stock[10];
    double price;
} StockPrice;


// --- GLOBÁLIS VÁLTOZÓK ---

// Buffer (Közös tárhely a Main és a Traderek között)
StockPrice price_buffer[BUFFER_SIZE];
int buffer_count = 0;
int buffer_read_idx = 0;
int buffer_write_idx = 0;

// Pénztárca és Részvények
double wallet_balance = INITIAL_BALANCE;
int stocks_owned = 0;

// Mutexek és Szinkronizáció
pthread_mutex_t wallet_mutex = PTHREAD_MUTEX_INITIALIZER;      // Pénz védelme
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;      // Buffer védelme
pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER; // Lista védelme
pthread_cond_t data_ready_cond = PTHREAD_COND_INITIALIZER;     // Jelző: van adat!

// Láncolt lista feje
Transaction* transaction_head = NULL;

// Vezérlés
volatile sig_atomic_t running = 1; // Fut-e a program?
pid_t market_pid;                  // A gyerek processz ID-ja


// --- FÜGGVÉNYEK ---

// Új tranzakció hozzáadása a láncolt listához
void add_transaction(const char* type, const char* stock, int qty, double price) {
    // 1. Memória foglalás (malloc)
    Transaction* new_node = (Transaction*)malloc(sizeof(Transaction));
    if (!new_node) return;

    // 2. Adatok kitöltése
    strcpy(new_node->type, type);
    strcpy(new_node->stock, stock);
    new_node->quantity = qty;
    new_node->price = price;

    // 3. Beszúrás a lista elejére (Mutex alatt!)
    pthread_mutex_lock(&transaction_mutex);
    new_node->next = transaction_head;
    transaction_head = new_node;
    pthread_mutex_unlock(&transaction_mutex);
}

// Tranzakciók kiírása (A végén hívjuk meg)
void print_transactions() {
    printf("\n--- TRANZAKCIO NAPLO ---\n");
    pthread_mutex_lock(&transaction_mutex);
    Transaction* current = transaction_head;
    while (current != NULL) {
        printf("[%s] %s | DB: %d | AR: %.2f $\n", 
               current->type, current->stock, current->quantity, current->price);
        current = current->next;
    }
    pthread_mutex_unlock(&transaction_mutex);
}

// Memória felszabadítás (Valgrind miatt kötelező!)
void free_transactions() {
    pthread_mutex_lock(&transaction_mutex);
    Transaction* current = transaction_head;
    while (current != NULL) {
        Transaction* temp = current;
        current = current->next;
        free(temp); // Felszabadítjuk az aktuális elemet
    }
    transaction_head = NULL;
    pthread_mutex_unlock(&transaction_mutex);
}

// Signal Handler (Ctrl+C esetén)
void sigint_handler(int sig) {
    printf("\n[SYSTEM] Leallitas kezdemenyezese...\n");
    running = 0;

    // Leállítjuk a gyerek processzt (Piac)
    if (market_pid > 0) {
        kill(market_pid, SIGTERM);
    }

    // Felébresztjük az alvó szálakat, hogy észrevegyék a leállást
    pthread_mutex_lock(&buffer_mutex);
    pthread_cond_broadcast(&data_ready_cond);
    pthread_mutex_unlock(&buffer_mutex);
}

// --- FOLYAMATOK ÉS SZÁLAK ---

// 1. PIAC FOLYAMAT (Gyerek)
// Véletlenszerű árakat generál és írja a Pipe-ba
void market_process(int pipe_write_fd) {
    srand(time(NULL) ^ getpid()); // Random seed
    char buffer[50];
    const char* stocks[] = {"AAPL", "GOOG", "TSLA", "MSFT", "AMZN"};

    while (running) {
        // Véletlen részvény és ár
        int stock_idx = rand() % 5;
        double price = 100.0 + (rand() % 400); // 100 - 500 $

        // Formázás: "NEV AR"
        snprintf(buffer, sizeof(buffer), "%s %.2f", stocks[stock_idx], price);

        // Írás a csőbe
        write(pipe_write_fd, buffer, strlen(buffer) + 1);
        
        // usleep(500000); // Fél másodperc szünet (gyorsabb teszteléshez)
        sleep(1); 
    }
    close(pipe_write_fd);
    exit(0);
}

// 2. KERESKEDŐ SZÁL (Trader Thread)
// Figyeli a buffert és vásárol
void* trader_thread(void* arg) {
    int id = *(int*)arg;
    free(arg); // Malloc-olt ID felszabadítása

    while (1) {
        StockPrice current_stock;

        // --- 1. Adat kivétele a Bufferből (Fogyasztó) ---
        pthread_mutex_lock(&buffer_mutex);
        
        // Várakozás, amíg üres a buffer ÉS fut a program
        while (buffer_count == 0 && running) {
            pthread_cond_wait(&data_ready_cond, &buffer_mutex);
        }

        // Ha leállás van és üres a buffer, kilépünk
        if (!running && buffer_count == 0) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        // Kivesszük az adatot (Circular Buffer logika)
        current_stock = price_buffer[buffer_read_idx];
        buffer_read_idx = (buffer_read_idx + 1) % BUFFER_SIZE;
        buffer_count--;

        pthread_mutex_unlock(&buffer_mutex);

        // --- 2. Kereskedési logika ---
        // Egyszerű stratégia: Ha van elég pénz, veszünk 1 db-ot
        pthread_mutex_lock(&wallet_mutex);
        
        if (wallet_balance >= current_stock.price) {
            wallet_balance -= current_stock.price;
            stocks_owned++;
            printf("[Trader %d] VASARLAS: %s - %.2f $ | Maradek: %.2f $\n", 
                   id, current_stock.stock, current_stock.price, wallet_balance);
            
            // Tranzakció naplózása (Lista)
            add_transaction("BUY", current_stock.stock, 1, current_stock.price);
        } else {
            printf("[Trader %d] NINCS PENZ: %s (%.2f $)\n", id, current_stock.stock, current_stock.price);
        }
        
        pthread_mutex_unlock(&wallet_mutex);
        
        usleep(100000 + (rand() % 200000)); // Kis szimulált gondolkodási idő
    }
    return NULL;
}

int main() {
    int pipe_fd[2];
    pthread_t traders[NUM_TRADERS];

    printf("========================================\n");
    printf("  WALL STREET - PARHUZAMOS TOZSDE\n");
    printf("========================================\n");
    
    // 1. Signal handler
    signal(SIGINT, sigint_handler);

    // 2. Pipe létrehozása
    if (pipe(pipe_fd) == -1) {
        perror("Pipe hiba");
        return 1;
    }

    // 3. Fork - Piac indítása
    market_pid = fork();
    if (market_pid < 0) {
        perror("Fork hiba");
        return 1;
    }

    if (market_pid == 0) {
        // --- GYEREK (Piac) ---
        close(pipe_fd[0]); // Olvasó vég bezárása
        market_process(pipe_fd[1]);
        return 0;
    }

    // --- SZÜLŐ (Main / Motor) ---
    close(pipe_fd[1]); // Író vég bezárása

    // 4. Kereskedő szálak indítása
    for (int i = 0; i < NUM_TRADERS; i++) {
        int* id = malloc(sizeof(int));
        *id = i + 1;
        pthread_create(&traders[i], NULL, trader_thread, id);
    }

    // 5. Master Ciklus (Olvassa a Pipe-ot)
    char raw_buffer[50];
    while (running) {
        // Blokkoló olvasás a Pipe-ból
        int bytes = read(pipe_fd[0], raw_buffer, sizeof(raw_buffer));
        
        if (bytes > 0) {
            // Adat feldolgozása
            char stock_name[10];
            double price;
            sscanf(raw_buffer, "%s %lf", stock_name, &price);

            // --- Betétel a Bufferbe (Termelő) ---
            pthread_mutex_lock(&buffer_mutex);
            
            if (buffer_count < BUFFER_SIZE) {
                // Van hely, betesszük
                strcpy(price_buffer[buffer_write_idx].stock, stock_name);
                price_buffer[buffer_write_idx].price = price;
                
                buffer_write_idx = (buffer_write_idx + 1) % BUFFER_SIZE;
                buffer_count++;
                
                // Ébresztjük a szálakat: "Hahó, van friss árfolyam!"
                pthread_cond_broadcast(&data_ready_cond);
            } else {
                printf("[MAIN] Buffer tele! Adat eldobva: %s\n", stock_name);
            }
            
            pthread_mutex_unlock(&buffer_mutex);
        } else {
            break; // Hiba vagy vége
        }
    }

    // 6. Takarítás (Cleanup)
    printf("\n[SYSTEM] Leallitas... Szalak bevarasa...\n");
    for (int i = 0; i < NUM_TRADERS; i++) {
        pthread_join(traders[i], NULL);
    }

    waitpid(market_pid, NULL, 0); // Piac folyamat bevárása
    close(pipe_fd[0]);            // Pipe bezárása

    printf("\n=== VEGSO EGYENLEG ===\n");
    printf("Egyenleg: %.2f $\n", wallet_balance);
    printf("Reszvenyek: %d db\n", stocks_owned);

    print_transactions();
    free_transactions(); // Lista törlése (Valgrind tiszta!)

    pthread_mutex_destroy(&wallet_mutex);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_mutex_destroy(&transaction_mutex);
    pthread_cond_destroy(&data_ready_cond);

    printf("[RENDSZER] Sikeres leallitas.\n");
    return 0;
}
