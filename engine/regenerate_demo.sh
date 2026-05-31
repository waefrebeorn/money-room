#!/bin/bash
# Regenerate demo_history.json from the CSV log
# Runs every 15 min via cron

CSV="/home/wubu2/.hermes/pm_logs/c_room/room_log.csv"
OUTDIR="/home/wubu2/money-room/docs/demos"

# Use awk to extract last 24h and write demo data
cd "$(dirname "$0")/.."

# Generate demo history from CSV
timeout 90 awk -F, '
BEGIN { cutoff = systime() - 86400; n = 0; }
!/^cycle/ && $2 >= cutoff && NF >= 13 {
    data[n++] = $0;
}
END {
    step = (n > 288) ? int(n / 288) : 1;
    system("mkdir -p '"$OUTDIR"'");
    out = "'"$OUTDIR"'/demo_history.json";
    
    printf "{\n" > out;
    printf "  \"generated_at\": %d,\n", systime() > out;
    printf "  \"cutoff_ts\": %d,\n", cutoff > out;
    printf "  \"rows_total\": %d,\n", n > out;
    printf "  \"rows_shown\": %d,\n", (n + step - 1) / step > out;
    printf "  \"step\": %d,\n", step > out;
    printf "  \"history\": [\n" > out;
    
    for (i = 0; i < n; i += step) {
        split(data[i], f, ",");
        printf "    [%s,%s,%s,%s,%.4f,%.4f,%.4f,%s,%.4f,%.4f]",
            f[1], f[2], f[4], f[5],
            f[6]+0, f[7]+0, f[8]+0,
            f[11], f[12]+0, f[13]+0 > out;
        if (i + step < n) printf "," > out;
        printf "\n" > out;
    }
    printf "  ]\n}\n" > out;
    close(out);
    printf "[DEMO] %d rows, %d sampled\n", n, (n+step-1)/step;
}' "$CSV" || echo "[DEMO] FAILED"
