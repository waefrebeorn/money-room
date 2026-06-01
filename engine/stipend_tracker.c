/**
 * stipend_tracker.c — Stipend Tracker ($1,760/month IRL bills)
 * Replaces stipend_tracker.py (181 lines Python)
 * Reads ecosystem state, tracks payouts, prints financial report.
 * Cron: daily at 9AM (no_agent mode)
 * Build: gcc -O2 stipend_tracker.c -o stipend_tracker -ljansson -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>

#define STIPEND_FILE "/home/wubu2/.hermes/pm_logs/stipend_log.json"
#define ECO_PF_FILE "/home/wubu2/.hermes/pm_logs/eco/portfolios.json"
#define ECO_ML_FILE "/home/wubu2/.hermes/pm_logs/eco/minute_log.jsonl"
#define STIPEND_MONTHLY 1760.0

typedef struct { const char *name; double threshold; } Milestone;
static const Milestone MILESTONES[] = {
    {"notify", 5.0}, {"seed", 50.0}, {"dump_safe", 100.0},
    {"milestone_1k", 1000.0}, {"debt_payoff", 20000.0}, {NULL, 0}};

typedef struct { const char *name; double amount; } BillItem;
static const BillItem BILLS[] = {
    {"Rent/Utilities", 680}, {"Starlink", 60}, {"Phone", 50},
    {"Credit Cards", 370}, {"PayPal", 200}, {"Car Payment", 400}, {NULL, 0}};

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f); buf[n] = 0;
    fclose(f); return buf;
}

static void load_eco_state(double *cash, double *pnl, int *n, int *tt, int *ot) {
    *cash = 0; *pnl = 0; *n = 0; *tt = 0; *ot = 0;
    char *raw = read_file(ECO_PF_FILE);
    if (!raw) return;
    json_error_t err;
    json_t *pf = json_loads(raw, 0, &err); free(raw);
    if (!pf || !json_is_object(pf)) { json_decref(pf); return; }
    const char *k; json_t *v;
    json_object_foreach(pf, k, v) {
        (*n)++;
        json_t *c = json_object_get(v, "cash");
        if (c) *cash += json_number_value(c);
        json_t *p = json_object_get(v, "total_pnl");
        if (p) *pnl += json_number_value(p);
    }
    json_decref(pf);
    raw = read_file(ECO_ML_FILE);
    if (!raw) return;
    char *last = NULL, *tok = strtok(raw, "\n");
    while (tok) { last = tok; tok = strtok(NULL, "\n"); }
    if (last) {
        json_t *ml = json_loads(last, 0, &err);
        if (ml) {
            json_t *t = json_object_get(ml, "total_trades");
            if (t) *tt = json_integer_value(t);
            json_t *o = json_object_get(ml, "open_trades");
            if (o) *ot = json_integer_value(o);
            json_decref(ml);
        }
    }
    free(raw);
}

static json_t *load_stipend_log(void) {
    char *raw = read_file(STIPEND_FILE);
    if (raw) {
        json_error_t err;
        json_t *j = json_loads(raw, 0, &err); free(raw);
        if (j) return j;
    }
    json_t *d = json_object();
    json_object_set_new(d, "created_at", json_real(time(NULL)));
    json_object_set_new(d, "total_earned_cumulative", json_real(0));
    json_object_set_new(d, "total_paid_out", json_real(0));
    json_object_set_new(d, "available_for_payout", json_real(0));
    json_object_set_new(d, "milestones_reached", json_array());
    json_object_set_new(d, "last_stipend_month", json_string(""));
    json_object_set_new(d, "stipend_remaining_this_month", json_real(STIPEND_MONTHLY));
    return d;
}

static void save_stipend_log(json_t *log) {
    char *out = json_dumps(log, JSON_INDENT(2));
    if (!out) return;
    mkdir("/home/wubu2/.hermes/pm_logs", 0755);
    FILE *f = fopen(STIPEND_FILE, "w");
    if (f) { fprintf(f, "%s\n", out); fclose(f); }
    free(out);
}

int main(void) {
    double total_cash, total_pnl;
    int n_agents, total_trades, open_trades;
    load_eco_state(&total_cash, &total_pnl, &n_agents, &total_trades, &open_trades);

    json_t *log = load_stipend_log();
    double cumulative = json_number_value(json_object_get(log, "total_earned_cumulative"));
    double paid_out = json_number_value(json_object_get(log, "total_paid_out"));
    const char *last_month = json_string_value(json_object_get(log, "last_stipend_month"));
    double remaining = json_number_value(json_object_get(log, "stipend_remaining_this_month"));

    double total_seed = n_agents * 1000.0;
    double cumulative_profit = (total_cash - total_seed) + total_pnl;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char curr_month[16];
    strftime(curr_month, sizeof(curr_month), "%Y-%m", tm);

    if (strcmp(last_month ? last_month : "", curr_month) != 0) {
        json_object_set_new(log, "last_stipend_month", json_string(curr_month));
        json_object_set_new(log, "stipend_remaining_this_month", json_real(STIPEND_MONTHLY));
        remaining = STIPEND_MONTHLY;
    }

    double new_profit = cumulative_profit - cumulative;
    if (new_profit > 0) {
        cumulative = cumulative_profit;
        json_object_set_new(log, "total_earned_cumulative", json_real(cumulative));
    }

    double earned_net = cumulative - paid_out;
    double available = fmin(earned_net, remaining);
    if (available < 0) available = 0;
    json_object_set_new(log, "available_for_payout", json_real(available));

    json_t *milestones = json_object_get(log, "milestones_reached");
    if (!milestones || !json_is_array(milestones)) {
        milestones = json_array();
        json_object_set_new(log, "milestones_reached", milestones);
    }

    char tb[32]; strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", tm);
    printf("============================================================\n");
    printf("  STIPEND TRACKER - Money Room\n  %s\n", tb);
    printf("============================================================\n");
    printf("  Ecosystem stats:\n");
    printf("    Total cash:     $%.0f\n", total_cash);
    printf("    Total PnL:      $%.0f\n", total_pnl);
    printf("    Mean PnL:       $%.2f\n", n_agents > 0 ? total_pnl / n_agents : 0);
    printf("    Total trades:   %d\n", total_trades);
    printf("\n  Cumulative profit:  $%.0f\n", cumulative);
    printf("  Available payout:   $%.2f\n", available);
    printf("  Monthly target:     $%.0f/month\n", STIPEND_MONTHLY);
    printf("  Remaining this mo:  $%.0f\n", remaining);

    for (int i = 0; MILESTONES[i].name; i++) {
        int found = 0;
        size_t idx; json_t *mval;
        json_array_foreach(milestones, idx, mval) {
            if (strcmp(json_string_value(mval), MILESTONES[i].name) == 0) { found = 1; break; }
        }
        if (cumulative >= MILESTONES[i].threshold && !found) {
            json_array_append_new(milestones, json_string(MILESTONES[i].name));
            printf("  MILESTONE: %s ($%.0f)\n", MILESTONES[i].name, MILESTONES[i].threshold);
        }
    }

    printf("\n  Monthly bills ($%.0f):\n", STIPEND_MONTHLY);
    for (int i = 0; BILLS[i].name; i++) {
        double pct = fmin(100.0, available / BILLS[i].amount * 100.0);
        printf("    %-16s $%5.0f  %5.1f%% covered\n", BILLS[i].name, BILLS[i].amount, pct);
    }

    save_stipend_log(log);
    json_decref(log);
    return 0;
}
