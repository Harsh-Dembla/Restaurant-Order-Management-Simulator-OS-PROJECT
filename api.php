<?php
/*
 * api.php — Thin PHP bridge between the web frontend and the C simulation.
 *
 * Endpoints (POST with JSON body):
 *   action: "status"          → read sim_state.json and return it
 *   action: "cancel"          → write cancel_cmd.txt with order id
 *   action: "speed"           → write speed_cmd.txt with float speed
 *
 * No database needed — all state lives in sim_state.json
 * written by the C process every 500ms.
 */

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { http_response_code(200); exit; }

$sim_dir = __DIR__;   /* adjust if C binary runs from a different directory */

/* ── Helpers ────────────────────────────────────────────────────────────── */
function json_resp($ok, $data = []) {
    echo json_encode(array_merge(['ok' => $ok], $data));
    exit;
}

function safe_file($path) {
    /* Prevent path traversal */
    $real = realpath(dirname($path));
    if ($real === false || strpos($real, realpath(__DIR__)) !== 0) {
        json_resp(false, ['error' => 'Invalid path']);
    }
}

/* ── Route ──────────────────────────────────────────────────────────────── */
$input  = json_decode(file_get_contents('php://input'), true) ?? [];
$action = $input['action'] ?? $_GET['action'] ?? 'status';

switch ($action) {

    /* ── GET sim status ─────────────────────────────────────────────────── */
    case 'status':
        $file = $sim_dir . '/sim_state.json';
        if (!file_exists($file)) {
            json_resp(false, ['error' => 'Simulation not running']);
        }
        $raw = file_get_contents($file);
        $data = json_decode($raw, true);
        if ($data === null) {
            json_resp(false, ['error' => 'State file corrupted']);
        }
        json_resp(true, ['state' => $data]);
        break;

    /* ── Cancel an order ────────────────────────────────────────────────── */
    case 'cancel':
        $id = intval($input['id'] ?? 0);
        if ($id <= 0) json_resp(false, ['error' => 'Invalid order id']);
        $file = $sim_dir . '/cancel_cmd.txt';
        safe_file($file);
        file_put_contents($file, $id);
        json_resp(true, ['message' => "Cancel request sent for order #$id"]);
        break;

    /* ── Set simulation speed ───────────────────────────────────────────── */
    case 'speed':
        $spd = floatval($input['speed'] ?? 1.0);
        if ($spd < 0.5 || $spd > 5.0) json_resp(false, ['error' => 'Speed out of range (0.5–5)']);
        $file = $sim_dir . '/speed_cmd.txt';
        safe_file($file);
        file_put_contents($file, $spd);
        json_resp(true, ['message' => "Speed set to {$spd}x"]);
        break;

    default:
        json_resp(false, ['error' => 'Unknown action']);
}
