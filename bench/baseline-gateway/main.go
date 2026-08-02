// Capsid M1B minimal Go gateway baseline (capsid-http-gw).
//
// This is the A/B baseline for the C++ capsid-host candidate: the same
// worker binary, the same bundle and the same workload, with the gateway
// layer implemented in Go via cgo over the capsid_worker ABI. It is
// deliberately minimal — path routing, GET/HEAD, one worker, per-request
// response assembly — so it measures the Go gateway baseline, not a full
// deployment surface.
//
// Component contract (see bench/run-ab.sh): CAPSID_BENCH_LISTEN,
// CAPSID_BENCH_READY_FD, CAPSID_BENCH_BUNDLE, CAPSID_BENCH_WORKER,
// CAPSID_BENCH_APPLICATION, CAPSID_BENCH_PUBLIC_AUTHORITY,
// CAPSID_BENCH_TIMEOUT_MS, CAPSID_BENCH_WINDOW, CAPSID_BENCH_SIDE,
// CAPSID_BENCH_CPU_PROFILE. The ready record reports the bundle/worker
// SHA-256 the runner uses for the A/B identity check.

package main

/*
#cgo LDFLAGS: -lcapsid_runtime -lstdc++ -lpthread
#include <poll.h>
#include <stdlib.h>
#include "capsid/runtime.h"
*/
import "C"

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime/pprof"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

const benchReadySchema = "bench-ready-v1"

var (
	apiMu sync.Mutex // serializes every capsid_worker call (single-owner rule)
	worker *C.capsid_worker
)

type bodyEvent struct {
	state *responseState
	body  []byte
}

type responseState struct {
	status     int
	headers    [][2]string
	headReady  chan struct{} // closed when the response head arrived
	bodyCh     chan []byte   // response body frames, drained by the HTTP handler
	endSeen    bool          // set by dispatchEnd; late credit is only ignored after this
	mu         sync.Mutex
}

type gateway struct {
	workerPath string
	bundle     []byte
	sourceName string
	application string
	publicAuthority string
	timeoutMs  uint64
	window     uint32
	nextID     uint64
	responses  map[uint64]*responseState
	responsesMu sync.Mutex
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "baseline-gateway: "+format+"\n", args...)
	os.Exit(1)
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func sha256File(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		fatal("cannot read %s: %v", path, err)
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// ---------------------------------------------------------------------------
// capsid_worker ABI wrappers (all calls must hold apiMu).
// ---------------------------------------------------------------------------

func workerNextEvent(event *C.capsid_event) C.capsid_result {
	event.struct_size = C.uint32_t(unsafe.Sizeof(C.capsid_event{}))
	return C.capsid_worker_next_event(worker, event)
}

func workerBeginRequest(id uint64, method, url string, headers []C.capsid_header) C.capsid_result {
	cMethod := C.CString(method)
	cURL := C.CString(url)
	defer C.free(unsafe.Pointer(cMethod))
	defer C.free(unsafe.Pointer(cURL))
	var headerPtr *C.capsid_header
	if len(headers) > 0 {
		headerPtr = &headers[0]
	}
	return C.capsid_worker_begin_request(worker, C.uint64_t(id), cMethod, cURL,
		headerPtr, C.size_t(len(headers)))
}

func workerBodylessRequest(id uint64, method, url string, headers []C.capsid_header) C.capsid_result {
	cMethod := C.CString(method)
	cURL := C.CString(url)
	defer C.free(unsafe.Pointer(cMethod))
	defer C.free(unsafe.Pointer(cURL))
	var headerPtr *C.capsid_header
	if len(headers) > 0 {
		headerPtr = &headers[0]
	}
	return C.capsid_worker_begin_bodyless_request(worker, C.uint64_t(id), cMethod, cURL,
		headerPtr, C.size_t(len(headers)))
}

// hopByHopHeaders are the fields the C++ host's M0 normalization strips
// before they reach the worker (plus Host, which is expressed by the URL).
// The baseline must forward exactly the same observable input so both sides
// present the worker with an identical header list.
var hopByHopHeaders = map[string]bool{
	"connection":          true,
	"keep-alive":          true,
	"proxy-connection":    true,
	"te":                  true,
	"trailer":             true,
	"transfer-encoding":   true,
	"upgrade":             true,
	"proxy-authenticate":  true,
	"proxy-authorization": true,
	"host":                true,
}

// requestHeaders converts the inbound request's end-to-end headers into the
// worker ABI form, mirroring the C++ host's normalized header snapshot. The
// returned C strings must be freed by the caller.
func requestHeaders(r *http.Request) ([]C.capsid_header, []*C.char) {
	headers := make([]C.capsid_header, 0, len(r.Header))
	var owned []*C.char
	for name, values := range r.Header {
		if hopByHopHeaders[strings.ToLower(name)] {
			continue
		}
		for _, value := range values {
			cName := C.CString(name)
			cValue := C.CString(value)
			owned = append(owned, cName, cValue)
			headers = append(headers, C.capsid_header{
				name: C.capsid_bytes{
					data: (*C.uint8_t)(unsafe.Pointer(cName)),
					size: C.size_t(len(name)),
				},
				value: C.capsid_bytes{
					data: (*C.uint8_t)(unsafe.Pointer(cValue)),
					size: C.size_t(len(value)),
				},
			})
		}
	}
	return headers, owned
}

func eventPayload(event *C.capsid_event) []byte {
	if event.payload.size == 0 {
		return nil
	}
	return C.GoBytes(unsafe.Pointer(event.payload.data), C.int(event.payload.size))
}

// ---------------------------------------------------------------------------
// Worker event loop: reads events and dispatches them to the waiting
// requests. Polls the worker fd (without holding apiMu) so command
// submissions from HTTP handlers are never starved.
// ---------------------------------------------------------------------------

func (g *gateway) eventLoop(ready chan<- string, done <-chan struct{}) {
	fd := int(C.capsid_worker_fd(worker))
	readySent := false

	for {
		select {
		case <-done:
			// main is destroying the worker; stop before the channel closes
			// so a closed IPC channel is not mistaken for a worker crash.
			return
		default:
		}
		var fds [1]C.struct_pollfd
		fds[0].fd = C.int(fd)
		fds[0].events = C.POLLIN
		n := C.poll(&fds[0], 1, 10)
		if n < 0 {
			// EINTR from Go-runtime signals is normal; retry.
			time.Sleep(time.Millisecond)
			continue
		}
		apiMu.Lock()
		var pendingBodies []bodyEvent
		var pendingEnds []uint64
		if n > 0 {
			for {
				var event C.capsid_event
				result := workerNextEvent(&event)
				if result == C.CAPSID_WOULD_BLOCK {
					break
				}
				if result != C.CAPSID_OK {
					select {
					case <-done:
						return
					default:
						fatal("next_event: %s", C.GoString(C.capsid_result_string(result)))
					}
				}
				if os.Getenv("CAPSID_BENCH_DEBUG") != "" {
					fmt.Fprintf(os.Stderr, "event type=%d id=%d\n",
						int(event._type), uint64(event.request_id))
				}
				switch event._type {
				case C.CAPSID_EVENT_READY:
					if os.Getenv("CAPSID_BENCH_DEBUG") != "" {
						fmt.Fprintf(os.Stderr, "READY branch entered\n")
					}
					if !readySent {
						payload := eventPayload(&event)
						var info C.capsid_build_info
						C.capsid_build_info_init(&info)
						if C.capsid_runtime_build_info(&info) == C.CAPSID_OK && info.compatibility_id != nil {
							expected := C.GoString(info.compatibility_id)
							if string(payload) != expected {
								fatal("worker compatibility ID mismatch")
							}
						}
						readySent = true
						close(ready)
					}
				case C.CAPSID_EVENT_REQUEST_CREDIT:
					// No request body in the M1B workloads.
				case C.CAPSID_EVENT_RESPONSE_HEAD:
					g.dispatchHead(uint64(event.request_id), int(event.status), &event)
				case C.CAPSID_EVENT_RESPONSE_BODY:
					// Body frames are handed to the handler only after apiMu
					// is released: the handler may be blocked writing to a
					// slow client while it waits for apiMu to return credit,
					// and a channel send under the lock would deadlock. The
					// state pointer is captured here (under responsesMu, which
					// the handlers also take) so the delivery below does not
					// depend on the responses map — dispatchEnd removes the
					// entry before the pending frames are delivered.
					g.responsesMu.Lock()
					state, ok := g.responses[uint64(event.request_id)]
					g.responsesMu.Unlock()
					if ok {
						pendingBodies = append(pendingBodies, bodyEvent{
							state: state,
							body:  eventPayload(&event),
						})
					}
				case C.CAPSID_EVENT_RESPONSE_END:
					// Deferred until after the pending body frames are
					// delivered, so a trailing frame that was read in the
					// same batch is not lost to the map removal.
					pendingEnds = append(pendingEnds, uint64(event.request_id))
				case C.CAPSID_EVENT_LOG, C.CAPSID_EVENT_ERROR:
					fmt.Fprintf(os.Stderr, "worker: %s\n", string(eventPayload(&event)))
				case C.CAPSID_EVENT_EXIT:
					if readySent {
						// The worker exiting after READY is the normal
						// shutdown path (main is destroying the worker).
						return
					}
					fatal("worker exited before READY")
				}
			}
		}
		apiMu.Unlock()
		// Deliver the body frames without holding apiMu, then close the
		// ended responses.
		for _, pending := range pendingBodies {
			pending.state.bodyCh <- pending.body
		}
		for _, id := range pendingEnds {
			g.dispatchEnd(id)
		}
	}
}

func (g *gateway) registerRequest(id uint64) *responseState {
	state := &responseState{
		headReady: make(chan struct{}),
		bodyCh:    make(chan []byte, 1),
	}
	g.responsesMu.Lock()
	g.responses[id] = state
	g.responsesMu.Unlock()
	return state
}

func (g *gateway) dispatchHead(id uint64, status int, event *C.capsid_event) {
	g.responsesMu.Lock()
	state, ok := g.responses[id]
	g.responsesMu.Unlock()
	if !ok {
		return
	}
	state.mu.Lock()
	state.status = status
	var count C.size_t
	if C.capsid_response_header_count(event, &count) == C.CAPSID_OK {
		for i := C.size_t(0); i < count; i++ {
			var header C.capsid_header
			if C.capsid_response_header_at(event, i, &header) != C.CAPSID_OK {
				continue
			}
			name := C.GoBytes(unsafe.Pointer(header.name.data), C.int(header.name.size))
			value := C.GoBytes(unsafe.Pointer(header.value.data), C.int(header.value.size))
			state.headers = append(state.headers, [2]string{string(name), string(value)})
		}
	}
	state.mu.Unlock()
	// The handler may start consuming the body as soon as the head is in:
	// waiting for RESPONSE_END would deadlock any response larger than the
	// credit window (the worker cannot send more body without credit, and
	// credit is only returned after the client writes).
	close(state.headReady)
}

func (g *gateway) dispatchEnd(id uint64) {
	g.responsesMu.Lock()
	state, ok := g.responses[id]
	if ok {
		state.mu.Lock()
		state.endSeen = true
		state.mu.Unlock()
		delete(g.responses, id)
	}
	g.responsesMu.Unlock()
	if ok {
		close(state.bodyCh)
	}
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

func (g *gateway) handle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	const prefix = "/@capsid/"
	rest, found := strings.CutPrefix(r.URL.Path, prefix)
	if !found {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	segments := strings.SplitN(rest, "/", 2)
	if len(segments) != 2 || segments[0] != g.application {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	workerPath := "/" + segments[1]
	url := "http://" + g.publicAuthority + workerPath

	g.responsesMu.Lock()
	g.nextID++
	id := g.nextID
	g.responsesMu.Unlock()
	state := g.registerRequest(id)

	apiMu.Lock()
	// Fairness: forward the same end-to-end header list the C++ host's M0
	// normalization keeps, so the worker observes identical input on both
	// sides (the header parsing cost must not be free on one side only).
	headers, headerStrings := requestHeaders(r)
	defer func() {
		for _, text := range headerStrings {
			C.free(unsafe.Pointer(text))
		}
	}()
	if result := workerBodylessRequest(id, r.Method, url, headers); result != C.CAPSID_OK {
		apiMu.Unlock()
		g.responsesMu.Lock()
		delete(g.responses, id)
		g.responsesMu.Unlock()
		http.Error(w, "worker unavailable", http.StatusBadGateway)
		return
	}
	if result := C.capsid_worker_flush(worker); result != C.CAPSID_OK &&
		result != C.CAPSID_WOULD_BLOCK {
		apiMu.Unlock()
		http.Error(w, "worker unavailable", http.StatusBadGateway)
		return
	}
	apiMu.Unlock()

	timeout := time.Duration(g.timeoutMs) * time.Millisecond
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	// The body must be consumed as soon as the head arrives: waiting for
	// RESPONSE_END would deadlock any response larger than the credit
	// window, because the worker cannot send more body until the client
	// write returns credit.
	select {
	case <-state.headReady:
	case <-time.After(timeout):
		g.failResponse(id, w, "worker request timeout", http.StatusGatewayTimeout)
		return
	}

	state.mu.Lock()
	status := state.status
	responseHeaders := state.headers
	state.mu.Unlock()
	if status == 0 {
		status = http.StatusBadGateway
	}
	for _, header := range responseHeaders {
		w.Header().Add(header[0], header[1])
	}
	w.WriteHeader(status)
	// Fairness: the C++ host streams the head immediately (async_write of
	// the header) instead of buffering the whole small response until the
	// handler returns; the baseline must present the same wire semantics.
	flusher, _ := w.(http.Flusher)
	if flusher != nil {
		flusher.Flush()
	}

	// Stream body frames to the client and return the response credit only
	// after each write succeeded — the same credit discipline as the C++
	// candidate host. A rejected grant or flush is a real state mismatch:
	// cancel the request instead of silently desynchronizing.
	for {
		select {
		case body, ok := <-state.bodyCh:
			if !ok {
				return // response ended
			}
			if r.Method != http.MethodHead {
				if _, err := w.Write(body); err != nil {
					g.cancelRequest(id)
					return
				}
				// Each frame streams immediately, like the C++ host's
				// per-block async_write.
				if flusher != nil {
					flusher.Flush()
				}
			}
			apiMu.Lock()
			grant := C.capsid_worker_grant_response_credit(worker,
				C.uint64_t(id), C.uint32_t(len(body)))
			flush := C.capsid_worker_flush(worker)
			apiMu.Unlock()
			if grant == C.CAPSID_INVALID_ARGUMENT {
				// Only accept a late credit after RESPONSE_END has been
				// observed: the Runtime erased the request and the grant is
				// moot (the same contract as the C++ host's end_seen guard).
				// Before end_seen, an INVALID_ARGUMENT is a real mismatch.
				state.mu.Lock()
				ended := state.endSeen
				state.mu.Unlock()
				if ended {
					return
				}
			}
			if grant != C.CAPSID_OK ||
				(flush != C.CAPSID_OK && flush != C.CAPSID_WOULD_BLOCK) {
				fmt.Fprintf(os.Stderr,
					"baseline-gateway: credit return failed: grant=%d flush=%d (request %d)\n",
					int(grant), int(flush), id)
				g.cancelRequest(id)
				return
			}
		case <-time.After(timeout):
			g.cancelRequest(id)
			return
		}
	}
}

// failResponse cancels the request and writes an error response.
func (g *gateway) failResponse(id uint64, w http.ResponseWriter, message string, status int) {
	g.responsesMu.Lock()
	delete(g.responses, id)
	g.responsesMu.Unlock()
	g.cancelRequest(id)
	http.Error(w, message, status)
}

func (g *gateway) cancelRequest(id uint64) {
	apiMu.Lock()
	C.capsid_worker_cancel(worker, C.uint64_t(id))
	C.capsid_worker_flush(worker)
	apiMu.Unlock()
}

func (g *gateway) readyRecord(port int, bundleSHA, workerSHA string) string {
	record, _ := json.Marshal(map[string]any{
		"schema":        benchReadySchema,
		"address":       "127.0.0.1",
		"port":          port,
		"bundle_sha256": bundleSHA,
		"worker_sha256": workerSHA,
	})
	return string(record)
}

func main() {
	workerPath := envOr("CAPSID_BENCH_WORKER", "")
	bundlePath := envOr("CAPSID_BENCH_BUNDLE", "")
	listen := envOr("CAPSID_BENCH_LISTEN", "127.0.0.1:0")
	readyFD, _ := strconv.Atoi(envOr("CAPSID_BENCH_READY_FD", "3"))
	application := envOr("CAPSID_BENCH_APPLICATION", "orders")
	authority := envOr("CAPSID_BENCH_PUBLIC_AUTHORITY", "public.example")
	timeoutMs, _ := strconv.ParseUint(envOr("CAPSID_BENCH_TIMEOUT_MS", "10000"), 10, 64)
	window64, _ := strconv.ParseUint(envOr("CAPSID_BENCH_WINDOW", "1024"), 10, 32)
	window := uint32(window64)
	if workerPath == "" || bundlePath == "" {
		fatal("CAPSID_BENCH_WORKER and CAPSID_BENCH_BUNDLE are required")
	}

	bundle, err := os.ReadFile(bundlePath)
	if err != nil {
		fatal("cannot read bundle: %v", err)
	}
	bundleSHA := sha256File(bundlePath)
	workerSHA := sha256File(workerPath)

	// Identity contract: report the bundle/worker SHA-256 before READY so
	// the runner can verify both sides loaded the same artifacts.
	if identityOut := os.Getenv("CAPSID_BENCH_IDENTITY_OUT"); identityOut != "" {
		identity, _ := json.Marshal(map[string]string{
			"bundle_sha256": bundleSHA,
			"worker_sha256": workerSHA,
		})
		if err := os.WriteFile(identityOut, identity, 0o644); err != nil {
			fatal("cannot write identity: %v", err)
		}
	}

	// CPU profile for the runner's profile run (absent in headline runs).
	if profilePath := os.Getenv("CAPSID_BENCH_CPU_PROFILE"); profilePath != "" {
		file, err := os.Create(profilePath)
		if err != nil {
			fatal("cannot create CPU profile: %v", err)
		}
		if err := pprof.StartCPUProfile(file); err != nil {
			fatal("cannot start CPU profile: %v", err)
		}
		defer pprof.StopCPUProfile()
	}

	var config C.capsid_worker_config
	C.capsid_worker_config_init(&config)
	cWorkerPath := C.CString(workerPath)
	defer C.free(unsafe.Pointer(cWorkerPath))
	config.worker_path = cWorkerPath
	config.request_timeout_ms = C.uint64_t(timeoutMs)
	config.initial_stream_window = C.uint32_t(window)
	config.max_inflight_requests = 128
	config.strict_sandbox = 0

	var spawned *C.capsid_worker
	if result := C.capsid_worker_spawn(&config, &spawned); result != C.CAPSID_OK {
		fatal("spawn worker: %s", C.GoString(C.capsid_result_string(result)))
	}
	worker = spawned
	defer C.capsid_worker_destroy(worker)

	sourceName := "file://" + bundlePath
	cSourceName := C.CString(sourceName)
	defer C.free(unsafe.Pointer(cSourceName))
	if result := C.capsid_worker_load_bundle_named(worker,
		(*C.uint8_t)(unsafe.Pointer(&bundle[0])), C.size_t(len(bundle)), cSourceName); result != C.CAPSID_OK {
		fatal("load bundle: %s", C.GoString(C.capsid_result_string(result)))
	}
	if result := C.capsid_worker_flush(worker); result != C.CAPSID_OK &&
		result != C.CAPSID_WOULD_BLOCK {
		fatal("flush bundle: %s", C.GoString(C.capsid_result_string(result)))
	}

	g := &gateway{
		workerPath:     workerPath,
		bundle:         bundle,
		sourceName:     sourceName,
		application:    application,
		publicAuthority: authority,
		timeoutMs:      timeoutMs,
		window:         window,
		nextID:         0,
		responses:      make(map[uint64]*responseState),
	}

	ready := make(chan string, 1)
	done := make(chan struct{})
	go g.eventLoop(ready, done)

	listener, err := net.Listen("tcp", listen)
	if err != nil {
		fatal("listen: %v", err)
	}
	port := listener.Addr().(*net.TCPAddr).Port

	// The worker must be READY before the ready record is published.
	<-ready

	server := &http.Server{Handler: http.HandlerFunc(g.handle)}
	go func() {
		_ = server.Serve(listener)
	}()

	record := g.readyRecord(port, bundleSHA, workerSHA)
	if os.Getenv("CAPSID_BENCH_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "writing ready record\n")
	}
	readyFile := os.NewFile(uintptr(readyFD), "ready")
	if readyFile == nil {
		fatal("ready fd %d is not open", readyFD)
	}
	if _, err := fmt.Fprintln(readyFile, record); err != nil {
		fatal("write ready record: %v", err)
	}
	readyFile.Close()
	if os.Getenv("CAPSID_BENCH_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "ready record written\n")
	}

	// SIGTERM is the runner's stop signal: stop the event loop, close the
	// server, and let the deferred worker destroy / CPU-profile flush run.
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGTERM, syscall.SIGINT)
	<-signals
	close(done)
	_ = server.Close()
}
