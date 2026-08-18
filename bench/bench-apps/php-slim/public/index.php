<?php
// Slim bench app — payloads/routes mirror the hono, flask, fastapi and
// sinatra fixtures (json/bytes/stream at 1k/8k/16k/32k). JSON bodies
// precomputed at boot; served as plain strings. bytes bodies precomputed;
// stream bodies write three chunks (X-Accel-Buffering: no lets nginx pass
// the chunks through instead of buffering the upstream response).
use Psr\Http\Message\ResponseInterface as Response;
use Psr\Http\Message\ServerRequestInterface as Request;
use Slim\Factory\AppFactory;

require __DIR__ . '/../vendor/autoload.php';

$app = AppFactory::create();

function bench_pad(int $size): string {
    return str_repeat('x', $size);
}

$J1K  = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => bench_pad(1024)]);
$J8K  = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => bench_pad(8192)]);
$J16K = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => bench_pad(16384)]);
$J32K = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => bench_pad(32768)]);
$J64  = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => bench_pad(65536)]);
$B1K  = str_repeat('a', 1024);
$FIXED = str_repeat('x', 1024);  // loadgen fixed-1k expects 0x78
$B8K  = str_repeat('a', 8192);
$B16K = str_repeat('a', 16384);
$B32K = str_repeat('a', 32768);
$MATRIX_SIZES = ['1k' => 1024, '4k' => 4096, '8k' => 8192, '16k' => 16384, '32k' => 32768, '64k' => 65536];
$MATRIX_PADS = [];
$MATRIX_BYTES = [];
foreach ($MATRIX_SIZES as $label => $size) {
    $MATRIX_PADS[$label] = str_repeat('x', $size - 11);
    $MATRIX_BYTES[$label] = str_repeat('b', $size);
}

$json = static function (Request $req, Response $res, string $body): Response {
    $res->getBody()->write($body);
    return $res->withHeader('Content-Type', 'application/json');
};
$octet = static function (Request $req, Response $res, string $body): Response {
    $res->getBody()->write($body);
    return $res->withHeader('Content-Type', 'application/octet-stream');
};
$stream = static function (Request $req, Response $res, int $size): Response {
    // Frozen stream contract: b*⌊n/3⌋ c*⌊n/3⌋ d*(rest) — the loadgen
    // verify cases pin these byte positions.
    $third = intdiv($size, 3);
    $body = $res->getBody();
    $body->write(str_repeat('b', $third));
    $body->write(str_repeat('c', $third));
    $body->write(str_repeat('d', $size - 2 * $third));
    return $res
        ->withHeader('Content-Type', 'application/octet-stream')
        ->withHeader('X-Accel-Buffering', 'no');
};

$app->get('/@capsid/orders/bench/json', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J1K']));
$app->get('/@capsid/orders/bench/json8k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J8K']));
$app->get('/@capsid/orders/bench/json16k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J16K']));
$app->get('/@capsid/orders/bench/json32k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J32K']));
$app->get('/@capsid/orders/bench/json64k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J64']));
$app->get('/@capsid/orders/bench/bytes', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['B1K']));
$app->get('/@capsid/orders/bench/bytes8k', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['B8K']));
$app->get('/@capsid/orders/bench/bytes16k', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['B16K']));
$app->get('/@capsid/orders/bench/bytes32k', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['B32K']));
$app->get('/@capsid/orders/bench/stream', static fn (Request $r, Response $res) => $stream($r, $res, 1024));
$app->get('/@capsid/orders/bench/stream8k', static fn (Request $r, Response $res) => $stream($r, $res, 8192));
$app->get('/@capsid/orders/bench/stream16k', static fn (Request $r, Response $res) => $stream($r, $res, 16384));
$app->get('/@capsid/orders/bench/stream32k', static fn (Request $r, Response $res) => $stream($r, $res, 32768));
$app->get('/@capsid/orders/fixed', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['FIXED']));

$app->get('/@capsid/orders/bench/matrix-json-{label}', static function (Request $req, Response $res, array $args): Response {
    $label = $args['label'];
    if (!isset($GLOBALS['MATRIX_SIZES'][$label])) {
        return $res->withStatus(404);
    }
    $size = $GLOBALS['MATRIX_SIZES'][$label];
    $body = json_encode(['data' => $GLOBALS['MATRIX_PADS'][$label]], JSON_UNESCAPED_SLASHES);
    $res->getBody()->write($body);
    return $res->withHeader('Content-Type', 'application/json')
               ->withHeader('Content-Length', (string) $size);
});

$app->get('/@capsid/orders/bench/matrix-bytes-{label}', static function (Request $req, Response $res, array $args): Response {
    $label = $args['label'];
    if (!isset($GLOBALS['MATRIX_SIZES'][$label])) {
        return $res->withStatus(404);
    }
    $size = $GLOBALS['MATRIX_SIZES'][$label];
    $res->getBody()->write($GLOBALS['MATRIX_BYTES'][$label]);
    return $res->withHeader('Content-Type', 'application/octet-stream')
               ->withHeader('Content-Length', (string) $size);
});

$app->get('/@capsid/orders/bench/matrix-stream-{label}', static function (Request $req, Response $res, array $args): Response {
    $label = $args['label'];
    if (!isset($GLOBALS['MATRIX_SIZES'][$label])) {
        return $res->withStatus(404);
    }
    $remaining = $GLOBALS['MATRIX_SIZES'][$label];
    $chunk = str_repeat('s', 4096);
    while ($remaining > 0) {
        $take = min($remaining, 4096);
        $res->getBody()->write(substr($chunk, 0, $take));
        $remaining -= $take;
    }
    return $res->withHeader('Content-Type', 'application/octet-stream');
});

$app->run();
