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

$app->run();
