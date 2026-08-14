<?php
// Slim bench app — payloads/routes mirror the hono, fastapi and sinatra
// fixtures. JSON bodies precomputed at boot; served as plain strings.
use Psr\Http\Message\ResponseInterface as Response;
use Psr\Http\Message\ServerRequestInterface as Request;
use Slim\Factory\AppFactory;

require __DIR__ . '/../vendor/autoload.php';

$app = AppFactory::create();

$J   = json_encode(['status' => 'ok', 'app' => 'slim', 'item' => 'benchmark', 'value' => 42]);
$J16 = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => str_repeat('x', 16384)]);
$J64 = json_encode(['status' => 'ok', 'app' => 'slim', 'pad' => str_repeat('x', 65536)]);
$B1K = str_repeat('x', 1024);
$MATRIX_SIZES = ['1k' => 1024, '4k' => 4096, '16k' => 16384, '32k' => 32768, '64k' => 65536];
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

$app->get('/@capsid/orders/bench/json', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J']));
$app->get('/@capsid/orders/bench/json16k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J16']));
$app->get('/@capsid/orders/bench/json64k', static fn (Request $r, Response $res) => $json($r, $res, $GLOBALS['J64']));
$app->get('/@capsid/orders/fixed', static fn (Request $r, Response $res) => $octet($r, $res, $GLOBALS['B1K']));

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
