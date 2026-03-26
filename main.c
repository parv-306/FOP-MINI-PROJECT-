#include "mongoose.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_http_addr = "http://0.0.0.0:8000";
static sqlite3 *db;

int process_order_db(const char *name, const char *prn, int item_id) {
    char sql[512];
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "INSERT INTO orders (customer_name, prn, item_id, status) VALUES ('%s', '%s', %d, 'Pending');", name, prn, item_id);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "UPDATE inventory SET stock = stock - 1 WHERE id = %d;", item_id);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    return 1;
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        sqlite3_stmt *res; 
        char json[4096];

        // 1. Get Inventory (Show Stock Availability)
        if (mg_match(hm->uri, mg_str("/get-inventory"), NULL)) {
            strcpy(json, "[");
            sqlite3_prepare_v2(db, "SELECT id, name, price, stock FROM inventory;", -1, &res, 0);
            while (sqlite3_step(res) == SQLITE_ROW) {
                char entry[512];
                snprintf(entry, sizeof(entry), "{\"id\": %d, \"name\": \"%s\", \"price\": %.2f, \"stock\": %d},", 
                         sqlite3_column_int(res, 0), sqlite3_column_text(res, 1), sqlite3_column_double(res, 2), sqlite3_column_int(res, 3));
                strcat(json, entry);
            }
            if (strlen(json) > 1) json[strlen(json) - 1] = ']'; else strcat(json, "]");
            sqlite3_finalize(res);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", json);
        }
        // 2. Place Order (Handles multiple Item IDs)
        else if (mg_match(hm->uri, mg_str("/place-order"), NULL)) {
            char name[64], prn[64], ids_str[512];
            mg_http_get_var(&hm->body, "name", name, sizeof(name));
            mg_http_get_var(&hm->body, "prn", prn, sizeof(prn));
            mg_http_get_var(&hm->body, "item_ids", ids_str, sizeof(ids_str));

            char *token = strtok(ids_str, ",");
            while (token != NULL) {
                process_order_db(name, prn, atoi(token));
                token = strtok(NULL, ",");
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\": \"Success\"}");
        }
        // 3. Get Orders (Grouped by Qty for Staff Dashboard)
        else if (mg_match(hm->uri, mg_str("/get-orders"), NULL)) {
            strcpy(json, "[");
            const char *sql = "SELECT o.customer_name, i.name, COUNT(i.id), o.prn "
                              "FROM orders o JOIN inventory i ON o.item_id = i.id "
                              "GROUP BY o.customer_name, i.name;";
            sqlite3_prepare_v2(db, sql, -1, &res, 0);
            while (sqlite3_step(res) == SQLITE_ROW) {
                char entry[512];
                snprintf(entry, sizeof(entry), "{\"customer\": \"%s\", \"item\": \"%s\", \"qty\": %d, \"prn\": \"%s\"},", 
                         sqlite3_column_text(res, 0), sqlite3_column_text(res, 1), sqlite3_column_int(res, 2), sqlite3_column_text(res, 3));
                strcat(json, entry);
            }
            if (strlen(json) > 1) json[strlen(json) - 1] = ']'; else strcat(json, "]");
            sqlite3_finalize(res);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", json);
        }
else if (mg_match(hm->uri, mg_str("/check-status"), NULL)) {
    char prn[64];
    mg_http_get_var(&hm->query, "prn", prn, sizeof(prn));

    sqlite3_stmt *res;
    const char *sql = "SELECT COUNT(*) FROM orders WHERE prn = ?;";
    sqlite3_prepare_v2(db, sql, -1, &res, 0);
    sqlite3_bind_text(res, 1, prn, -1, SQLITE_STATIC);
    
    int count = 0;
    if (sqlite3_step(res) == SQLITE_ROW) {
        count = sqlite3_column_int(res, 0);
    }
    sqlite3_finalize(res);

    // If count is 0, it means the order was deleted (Marked Ready)
    if (count == 0) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ready\": true}");
    } else {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ready\": false}");
    }
}
        // 4. Update Status (Delete Order on Ready)
else if (mg_match(hm->uri, mg_str("/update-status"), NULL)) {
    char name[64], item[64];
    
    // 1. Extract the data sent from the staff dashboard
    mg_http_get_var(&hm->body, "customer", name, sizeof(name));
    mg_http_get_var(&hm->body, "item_name", item, sizeof(item));

    // 2. Build the DELETE query
    // This removes all 'Pending' rows for this customer and specific item
    char *sql = sqlite3_mprintf(
        "DELETE FROM orders WHERE customer_name = '%q' AND item_id = "
        "(SELECT id FROM inventory WHERE name = '%q' LIMIT 1);", 
        name, item);
             
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql); // Important: free the mprintf memory

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\": \"Deleted\"}");
}
        else {
            struct mg_http_serve_opts opts = {.root_dir = "."};
            mg_http_serve_dir(c, hm, &opts);
        }
    }
}

int main() {
    sqlite3_open("cafe.db", &db);
    struct mg_mgr mgr; mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s_http_addr, fn, NULL);
    printf("Server running at %s\n", s_http_addr);
    for (;;) mg_mgr_poll(&mgr, 1000);
    return 0;
}