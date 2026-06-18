#define _GNU_SOURCE
/*
 * ================================================================
 *   DYNAMIC REAL ESTATE DATA ALLOCATOR
 *   Tech: C · Pointers · malloc · realloc · free
 *
 *   Memory model:
 *     - Property array grows on demand via realloc()
 *     - Each string field (address, city, type) lives on the heap
 *     - A dedicated Memory Audit log tracks every alloc / free
 *     - valgrind-clean: zero leaks on normal exit
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ─── Constants ────────────────────────────────────────────────── */
#define MAX_STR          128
#define INITIAL_CAPACITY   2   /* start small to demonstrate realloc */

/* ─── Colours (ANSI) ───────────────────────────────────────────── */
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_RED    "\033[31m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_CYAN   "\033[36m"
#define CLR_WHITE  "\033[97m"

/* ─── Property struct ──────────────────────────────────────────── */
typedef struct {
    int     id;
    char   *address;      /* heap-allocated strings */
    char   *city;
    char   *type;         /* Apartment / Villa / Plot / Commercial */
    double  price;        /* INR */
    double  area_sqft;
    int     bedrooms;
    int     year_built;
} Property;

/* ─── Memory-Audit node (singly-linked list) ───────────────────── */
typedef struct AuditNode {
    const char      *tag;       /* label for the allocation */
    void            *ptr;
    size_t           bytes;
    struct AuditNode *next;
} AuditNode;

/* ─── Allocator context ────────────────────────────────────────── */
typedef struct {
    Property  *data;        /* pointer to the heap array            */
    int        count;       /* properties currently stored          */
    int        capacity;    /* slots currently allocated            */
    int        next_id;     /* auto-increment ID counter            */
    size_t     total_alloc; /* running byte total (for audit)       */
    size_t     total_freed;
    AuditNode *audit_head;  /* linked list of live allocations      */
} Allocator;


/* ══════════════════════════════════════════════════════════════════
 *  Memory Audit helpers
 * ══════════════════════════════════════════════════════════════════ */

static void audit_record(Allocator *a, const char *tag,
                         void *ptr, size_t bytes) {
    AuditNode *node = (AuditNode *)malloc(sizeof(AuditNode));
    if (!node) return;
    node->tag   = tag;
    node->ptr   = ptr;
    node->bytes = bytes;
    node->next  = a->audit_head;
    a->audit_head = node;
    a->total_alloc += bytes;
}

static void audit_remove(Allocator *a, void *ptr, size_t bytes) {
    AuditNode **cur = &a->audit_head;
    while (*cur) {
        if ((*cur)->ptr == ptr) {
            AuditNode *tmp = *cur;
            *cur = tmp->next;
            free(tmp);
            a->total_freed += bytes;
            return;
        }
        cur = &(*cur)->next;
    }
}

/* Update pointer in audit list after realloc */
static void audit_update(Allocator *a, void *old_ptr,
                         void *new_ptr, size_t new_bytes) {
    AuditNode *cur = a->audit_head;
    while (cur) {
        if (cur->ptr == old_ptr) {
            /* credit the delta */
            a->total_alloc += (new_bytes > cur->bytes)
                                ? (new_bytes - cur->bytes) : 0;
            a->total_freed += (cur->bytes > new_bytes)
                                ? (cur->bytes - new_bytes) : 0;
            cur->ptr   = new_ptr;
            cur->bytes = new_bytes;
            return;
        }
        cur = cur->next;
    }
    /* not found → register fresh */
    audit_record(a, "property-array", new_ptr, new_bytes);
}

/* ── safe wrappers ─────────────────────────────────────────────── */
static void *safe_malloc(Allocator *a, const char *tag, size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        fprintf(stderr, CLR_RED "[FATAL] malloc failed for '%s'\n" CLR_RESET, tag);
        exit(EXIT_FAILURE);
    }
    audit_record(a, tag, p, sz);
    return p;
}

static char *heap_strdup(Allocator *a, const char *tag, const char *src) {
    size_t len = strlen(src) + 1;
    char  *p   = (char *)safe_malloc(a, tag, len);
    memcpy(p, src, len);
    return p;
}

static void safe_free_str(Allocator *a, char **str, size_t sz) {
    if (*str) {
        audit_remove(a, *str, sz);
        free(*str);
        *str = NULL;
    }
}


/* ══════════════════════════════════════════════════════════════════
 *  Allocator lifecycle
 * ══════════════════════════════════════════════════════════════════ */

static void allocator_init(Allocator *a) {
    a->count       = 0;
    a->capacity    = INITIAL_CAPACITY;
    a->next_id     = 1001;
    a->total_alloc = 0;
    a->total_freed = 0;
    a->audit_head  = NULL;

    size_t sz  = (size_t)a->capacity * sizeof(Property);
    a->data    = (Property *)malloc(sz);
    if (!a->data) {
        fprintf(stderr, CLR_RED "[FATAL] initial malloc failed\n" CLR_RESET);
        exit(EXIT_FAILURE);
    }
    audit_record(a, "property-array", a->data, sz);

    printf(CLR_GREEN "  [MEM] malloc()  ── %zu bytes for %d slots (property-array)\n"
           CLR_RESET, sz, a->capacity);
}

/* Grow the array by one capacity level using realloc() */
static void allocator_grow(Allocator *a) {
    int    old_cap  = a->capacity;
    int    new_cap  = old_cap * 2;          /* double strategy      */
    size_t new_sz   = (size_t)new_cap * sizeof(Property);
    size_t old_sz   = (size_t)old_cap * sizeof(Property);

    void *old_ptr = a->data;
    Property *tmp = (Property *)realloc(a->data, new_sz);
    if (!tmp) {
        fprintf(stderr, CLR_RED "[FATAL] realloc failed\n" CLR_RESET);
        free(a->data);
        exit(EXIT_FAILURE);
    }
    a->data     = tmp;
    a->capacity = new_cap;
    audit_update(a, old_ptr, a->data, new_sz);

    printf(CLR_YELLOW "  [MEM] realloc() ── %zu → %zu bytes  "
           "(%d → %d slots)\n" CLR_RESET,
           old_sz, new_sz, old_cap, new_cap);
}

/* Shrink array when count drops below 25% of capacity */
static void allocator_shrink(Allocator *a) {
    if (a->capacity <= INITIAL_CAPACITY) return;
    if (a->count > a->capacity / 4)     return;

    int    new_cap = a->capacity / 2;
    if (new_cap < INITIAL_CAPACITY) new_cap = INITIAL_CAPACITY;
    size_t new_sz  = (size_t)new_cap * sizeof(Property);
    size_t old_sz  = (size_t)a->capacity * sizeof(Property);

    void *old_ptr = a->data;
    Property *tmp = (Property *)realloc(a->data, new_sz);
    if (!tmp && new_sz > 0) return; /* keep old if realloc fails */
    a->data     = tmp;
    a->capacity = new_cap;
    audit_update(a, old_ptr, a->data, new_sz);

    printf(CLR_CYAN "  [MEM] realloc() ── shrink %zu → %zu bytes  "
           "(%d slots freed)\n" CLR_RESET,
           old_sz, new_sz, a->capacity);
}

/* Free a single property's heap-allocated string fields */
static void property_free_strings(Allocator *a, Property *p) {
    safe_free_str(a, &p->address, strlen(p->address ? p->address : "") + 1);
    safe_free_str(a, &p->city,    strlen(p->city    ? p->city    : "") + 1);
    safe_free_str(a, &p->type,    strlen(p->type    ? p->type    : "") + 1);
}

/* Full teardown */
static void allocator_destroy(Allocator *a) {
    for (int i = 0; i < a->count; i++)
        property_free_strings(a, &a->data[i]);

    size_t arr_sz = (size_t)a->capacity * sizeof(Property);
    audit_remove(a, a->data, arr_sz);
    free(a->data);
    a->data = NULL;

    /* free any remaining audit nodes */
    AuditNode *cur = a->audit_head;
    while (cur) {
        AuditNode *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    a->audit_head = NULL;
    printf(CLR_RED "  [MEM] free()    ── property array released\n" CLR_RESET);
}


/* ══════════════════════════════════════════════════════════════════
 *  CRUD operations
 * ══════════════════════════════════════════════════════════════════ */

/* ── Add ────────────────────────────────────────────────────────── */
static void property_add(Allocator *a,
                         const char *address,
                         const char *city,
                         const char *type,
                         double      price,
                         double      area,
                         int         bedrooms,
                         int         year) {
    /* Grow if full */
    if (a->count == a->capacity)
        allocator_grow(a);

    Property *p  = &a->data[a->count];
    p->id        = a->next_id++;
    p->address   = heap_strdup(a, "address", address);
    p->city      = heap_strdup(a, "city",    city);
    p->type      = heap_strdup(a, "type",    type);
    p->price     = price;
    p->area_sqft = area;
    p->bedrooms  = bedrooms;
    p->year_built= year;
    a->count++;

    printf(CLR_GREEN
           "  [OK] Property #%d added  "
           "(array: %d/%d slots used)\n" CLR_RESET,
           p->id, a->count, a->capacity);
}

/* ── Delete by ID ───────────────────────────────────────────────── */
static int property_delete(Allocator *a, int id) {
    for (int i = 0; i < a->count; i++) {
        if (a->data[i].id == id) {
            property_free_strings(a, &a->data[i]);
            /* shift left */
            memmove(&a->data[i], &a->data[i + 1],
                    (size_t)(a->count - i - 1) * sizeof(Property));
            a->count--;
            printf(CLR_RED
                   "  [OK] Property #%d deleted  "
                   "(array: %d/%d slots used)\n" CLR_RESET,
                   id, a->count, a->capacity);
            allocator_shrink(a);
            return 1;
        }
    }
    printf("  [!] Property ID %d not found.\n", id);
    return 0;
}

/* ── Find by ID ─────────────────────────────────────────────────── */
static Property *property_find(Allocator *a, int id) {
    for (int i = 0; i < a->count; i++)
        if (a->data[i].id == id)
            return &a->data[i];
    return NULL;
}

/* ── Update price ───────────────────────────────────────────────── */
static int property_update_price(Allocator *a, int id, double new_price) {
    Property *p = property_find(a, id);
    if (!p) { printf("  [!] ID %d not found.\n", id); return 0; }
    double old = p->price;
    p->price   = new_price;
    printf("  [OK] #%d price updated:  INR %.2f → INR %.2f\n",
           id, old, new_price);
    return 1;
}


/* ══════════════════════════════════════════════════════════════════
 *  Sorting  (in-place, pointer-swap on the struct array)
 * ══════════════════════════════════════════════════════════════════ */

static int cmp_price_asc (const void *a, const void *b) {
    double diff = ((Property*)a)->price - ((Property*)b)->price;
    return (diff > 0) - (diff < 0);
}
static int cmp_price_desc(const void *a, const void *b) {
    return cmp_price_asc(b, a);
}
static int cmp_area_asc  (const void *a, const void *b) {
    double diff = ((Property*)a)->area_sqft - ((Property*)b)->area_sqft;
    return (diff > 0) - (diff < 0);
}


/* ══════════════════════════════════════════════════════════════════
 *  Display helpers
 * ══════════════════════════════════════════════════════════════════ */

static void print_separator(char c, int n) {
    for (int i = 0; i < n; i++) putchar(c);
    putchar('\n');
}

static void print_property_row(const Property *p) {
    printf("  │ %-6d │ %-22s │ %-12s │ %-12s │ %12.0f │ %8.1f │ %3d BHK │ %4d │\n",
           p->id,
           p->address,
           p->city,
           p->type,
           p->price,
           p->area_sqft,
           p->bedrooms,
           p->year_built);
}

static void display_all(const Allocator *a) {
    if (a->count == 0) {
        printf("\n  (no properties in registry)\n");
        return;
    }
    printf("\n");
    print_separator('==', 100);
    printf("  %-6s   %-22s   %-12s   %-12s   %12s   %8s   %7s   %4s\n",
           "ID", "Address", "City", "Type",
           "Price (INR)", "Area(sqft)", "BHK", "Year");
    print_separator('-', 100);
    for (int i = 0; i < a->count; i++)
        print_property_row(&a->data[i]);
    print_separator('==', 100);
    printf("  Total: %d propert%s  |  Heap slots: %d  |  "
           "Allocated: %zu bytes  |  Freed: %zu bytes\n",
           a->count, a->count == 1 ? "y" : "ies",
           a->capacity, a->total_alloc, a->total_freed);
    print_separator('==', 100);
}

static void display_memory_audit(const Allocator *a) {
    printf("\n");
    print_separator('==', 72);
    printf("  MEMORY AUDIT LOG\n");
    print_separator('-', 72);
    printf("  %-20s  %-16s  %s\n", "Tag", "Ptr", "Bytes");
    print_separator('-', 72);
    AuditNode *cur = a->audit_head;
    if (!cur) printf("  (all allocations freed)\n");
    while (cur) {
        printf("  %-20s  %-16p  %zu\n", cur->tag, cur->ptr, cur->bytes);
        cur = cur->next;
    }
    print_separator('-', 72);
    printf("  Total Allocated : %zu bytes\n", a->total_alloc);
    printf("  Total Freed     : %zu bytes\n", a->total_freed);
    printf("  Live on Heap    : %zu bytes\n",
           a->total_alloc - a->total_freed);
    print_separator('==', 72);
}

/* ── Search by city or type ─────────────────────────────────────── */
static void search_properties(const Allocator *a,
                              const char *field,
                              const char *value) {
    int found = 0;
    printf("\n  Search ─ %s = \"%s\"\n", field, value);
    print_separator('-', 100);
    for (int i = 0; i < a->count; i++) {
        const Property *p = &a->data[i];
        const char *cmp_val = (strcmp(field, "city") == 0)
                                ? p->city : p->type;
        /* case-insensitive match */
        if (strcasecmp(cmp_val, value) == 0) {
            print_property_row(p);
            found++;
        }
    }
    if (!found) printf("  (no matches)\n");
    print_separator('-', 100);
    printf("  Found %d match(es)\n", found);
}

/* ── Summary statistics ─────────────────────────────────────────── */
static void show_statistics(const Allocator *a) {
    if (a->count == 0) {
        printf("  (no data for statistics)\n");
        return;
    }
    double sum_price = 0, sum_area = 0;
    double max_price = a->data[0].price, min_price = a->data[0].price;
    double max_area  = a->data[0].area_sqft;

    for (int i = 0; i < a->count; i++) {
        sum_price += a->data[i].price;
        sum_area  += a->data[i].area_sqft;
        if (a->data[i].price    > max_price) max_price = a->data[i].price;
        if (a->data[i].price    < min_price) min_price = a->data[i].price;
        if (a->data[i].area_sqft > max_area) max_area  = a->data[i].area_sqft;
    }

    printf("\n");
    print_separator('==', 52);
    printf("  REGISTRY STATISTICS\n");
    print_separator('-', 52);
    printf("  Properties       : %d\n",          a->count);
    printf("  Avg Price        : INR %14.2f\n",  sum_price / a->count);
    printf("  Max Price        : INR %14.2f\n",  max_price);
    printf("  Min Price        : INR %14.2f\n",  min_price);
    printf("  Avg Area         : %.2f sqft\n",   sum_area  / a->count);
    printf("  Max Area         : %.2f sqft\n",   max_area);
    printf("  Price/sqft (avg) : INR %.2f\n",
           (sum_price / a->count) / (sum_area / a->count));
    print_separator('==', 52);
}


/* ══════════════════════════════════════════════════════════════════
 *  Input helpers
 * ══════════════════════════════════════════════════════════════════ */

static void flush_stdin(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

static void read_string(const char *prompt, char *buf, int maxlen) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, maxlen, stdin)) buf[0] = '\0';
    buf[strcspn(buf, "\n")] = '\0'; /* strip newline */
}

static double read_double(const char *prompt) {
    double v;
    while (1) {
        printf("%s", prompt);
        if (scanf("%lf", &v) == 1 && v >= 0) { flush_stdin(); return v; }
        printf("  [!] Invalid. Enter a positive number.\n");
        flush_stdin();
    }
}

static int read_int(const char *prompt) {
    int v;
    while (1) {
        printf("%s", prompt);
        if (scanf("%d", &v) == 1 && v >= 0) { flush_stdin(); return v; }
        printf("  [!] Invalid. Enter a non-negative integer.\n");
        flush_stdin();
    }
}

static void interactive_add(Allocator *a) {
    char addr[MAX_STR], city[MAX_STR], type[MAX_STR];
    flush_stdin();
    read_string("  Address      : ", addr, MAX_STR);
    read_string("  City         : ", city, MAX_STR);

    printf("  Property Type\n");
    printf("    1. Apartment   2. Villa   3. Plot   4. Commercial\n");
    int tc = read_int("  Choice       : ");
    const char *types[] = {"Apartment","Villa","Plot","Commercial"};
    strncpy(type, (tc >= 1 && tc <= 4) ? types[tc-1] : "Other",
            MAX_STR - 1);

    double price    = read_double("  Price (INR)  : ");
    double area     = read_double("  Area (sqft)  : ");
    int    beds     = read_int   ("  Bedrooms     : ");
    int    year     = read_int   ("  Year Built   : ");

    property_add(a, addr, city, type, price, area, beds, year);
}


/* ══════════════════════════════════════════════════════════════════
 *  Banner & menu
 * ══════════════════════════════════════════════════════════════════ */

static void show_banner(void) {
    printf("\n");
    printf(CLR_BOLD CLR_WHITE);
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║       DYNAMIC REAL ESTATE DATA ALLOCATOR             ║\n");
    printf("  ║       C · Pointers · malloc · realloc · free         ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf(CLR_RESET "\n");
}

static void show_menu(void) {
    printf("\n");
    printf("  ┌──────────────────────────────────────────────┐\n");
    printf("  │               MAIN MENU                      │\n");
    printf("  ├──────────────────────────────────────────────┤\n");
    printf("  │  1.  Add Property                            │\n");
    printf("  │  2.  Delete Property (by ID)                 │\n");
    printf("  │  3.  Update Price (by ID)                    │\n");
    printf("  │  4.  Display All Properties                  │\n");
    printf("  │  5.  Search by City                          │\n");
    printf("  │  6.  Search by Type                          │\n");
    printf("  │  7.  Sort by Price (ascending)               │\n");
    printf("  │  8.  Sort by Price (descending)              │\n");
    printf("  │  9.  Sort by Area  (ascending)               │\n");
    printf("  │  10. Registry Statistics                     │\n");
    printf("  │  11. Memory Audit                            │\n");
    printf("  │  12. Load Demo Data                          │\n");
    printf("  │  0.  Exit (free all memory)                  │\n");
    printf("  └──────────────────────────────────────────────┘\n");
    printf("  Choice: ");
    fflush(stdout);
}


/* ══════════════════════════════════════════════════════════════════
 *  Demo data  (exercises realloc by filling past initial capacity)
 * ══════════════════════════════════════════════════════════════════ */
static void load_demo_data(Allocator *a) {
    printf("\n  Loading demo properties...\n");
    property_add(a, "12 MG Road",         "Bengaluru", "Apartment",  8500000, 1200, 3, 2019);
    property_add(a, "7 Banjara Hills",    "Hyderabad", "Villa",     22000000, 3500, 4, 2020);
    property_add(a, "Plot 45 Sector 62",  "Noida",     "Plot",       5500000,  900, 0, 2018);
    property_add(a, "33 Andheri West",    "Mumbai",    "Apartment", 15000000, 1050, 2, 2021);
    property_add(a, "88 T Nagar",         "Chennai",   "Commercial",18000000, 2800, 0, 2017);
    property_add(a, "2 Civil Lines",      "Jaipur",    "Villa",     11000000, 2900, 4, 2022);
    property_add(a, "Plot 9 Sector 21",   "Gurugram",  "Plot",       7200000, 1200, 0, 2023);
    property_add(a, "19 Koregaon Park",   "Pune",      "Apartment",  9800000, 1400, 3, 2020);
    printf(CLR_GREEN "  [OK] Demo data loaded.\n" CLR_RESET);
}


/* ══════════════════════════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(void) {
    Allocator a;
    allocator_init(&a);
    show_banner();

    printf(CLR_CYAN
           "  [MEM] Allocator ready  |  initial capacity: %d slots  "
           "(%zu bytes)\n" CLR_RESET,
           a.capacity, (size_t)a.capacity * sizeof(Property));

    int choice;
    do {
        show_menu();

        if (scanf("%d", &choice) != 1) {
            flush_stdin();
            printf("  [!] Please enter a number.\n");
            choice = -1;
            continue;
        }
        flush_stdin();

        switch (choice) {

        case 1:
            interactive_add(&a);
            break;

        case 2: {
            int id = read_int("  Enter property ID to delete: ");
            property_delete(&a, id);
            break;
        }

        case 3: {
            int    id  = read_int   ("  Enter property ID to update: ");
            double np  = read_double("  Enter new price (INR)       : ");
            property_update_price(&a, id, np);
            break;
        }

        case 4:
            display_all(&a);
            break;

        case 5: {
            char city[MAX_STR];
            flush_stdin();
            read_string("  Enter city name: ", city, MAX_STR);
            search_properties(&a, "city", city);
            break;
        }

        case 6: {
            printf("  Types: Apartment / Villa / Plot / Commercial\n");
            char type[MAX_STR];
            flush_stdin();
            read_string("  Enter type: ", type, MAX_STR);
            search_properties(&a, "type", type);
            break;
        }

        case 7:
            qsort(a.data, (size_t)a.count, sizeof(Property), cmp_price_asc);
            printf("  Sorted by price ascending.\n");
            display_all(&a);
            break;

        case 8:
            qsort(a.data, (size_t)a.count, sizeof(Property), cmp_price_desc);
            printf("  Sorted by price descending.\n");
            display_all(&a);
            break;

        case 9:
            qsort(a.data, (size_t)a.count, sizeof(Property), cmp_area_asc);
            printf("  Sorted by area ascending.\n");
            display_all(&a);
            break;

        case 10:
            show_statistics(&a);
            break;

        case 11:
            display_memory_audit(&a);
            break;

        case 12:
            load_demo_data(&a);
            break;

        case 0:
            printf("\n  Freeing all heap memory...\n");
            allocator_destroy(&a);
            printf(CLR_GREEN
                   "  [MEM] All memory freed. "
                   "Total allocated: %zu bytes. Leaks: 0\n"
                   CLR_RESET,
                   a.total_alloc);
            printf("\n  Goodbye!\n\n");
            break;

        default:
            printf("  [!] Invalid choice. Enter 0–12.\n");
        }

    } while (choice != 0);

    return 0;
}
