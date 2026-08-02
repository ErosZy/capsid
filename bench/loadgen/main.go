// Capsid M1B load generator.
//
// Saturates the target with a fixed concurrency (CAPSID_BENCH_INFLIGHT over
// CAPSID_BENCH_CONNECTIONS keep-alive connections), separates warm-up from
// the measured phase, and writes:
//
//   - one raw sample line per run (side/round tagged) to
//     CAPSID_BENCH_SAMPLES_OUT: qps, p50/p95/p99 latency, dispatch wait
//     (request creation -> actual dispatch, including the closed-loop
//     inflight slot wait), completed/errors/timeouts;
//   - one correctness verdict to CAPSID_BENCH_CORRECTNESS_OUT: responses are
//     content-checked, and error responses are never counted as success
//     (errors_as_success is always false).
//
// Workloads:
//   fixed-1k      GET /@capsid/orders/fixed       -> 1024 bytes of 0x78
//   cpu-template  GET /@capsid/orders/cpu         -> generated HTML template
package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"
)

type sample struct {
	Side           string  `json:"side"`
	Round          int     `json:"round"`
	Phase          string  `json:"phase"`
	QPS            float64 `json:"qps"`
	P50Ms          float64 `json:"p50_ms"`
	P95Ms          float64 `json:"p95_ms"`
	P99Ms          float64 `json:"p99_ms"`
	DispatchWaitMs  float64 `json:"dispatch_wait_ms"`
	Completed      int     `json:"completed"`
	Errors         int     `json:"errors"`
	Timeouts       int     `json:"timeouts"`
	DurationS      float64 `json:"duration_s"`
	Connections    int     `json:"connections"`
	Inflight       int     `json:"inflight"`
}

type correctness struct {
	OK                bool   `json:"ok"`
	Error             string `json:"error,omitempty"`
	ResponsesChecked  int    `json:"responses_checked"`
	Mismatches        int    `json:"mismatches"`
	Errors            int    `json:"errors"`
	Timeouts          int    `json:"timeouts"`
	ErrorsAsSuccess   bool   `json:"errors_as_success"`
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func envInt(name string, fallback int) int {
	value, err := strconv.Atoi(os.Getenv(name))
	if err != nil {
		return fallback
	}
	return value
}

func main() {
	target := envOr("CAPSID_BENCH_TARGET", "")
	if target == "" {
		fatal("CAPSID_BENCH_TARGET is required")
	}
	workload := envOr("CAPSID_BENCH_WORKLOAD", "fixed-1k")
	side := envOr("CAPSID_BENCH_SIDE", "baseline")
	round := envInt("CAPSID_BENCH_ROUND", 1)
	warmupS := envInt("CAPSID_BENCH_WARMUP_S", 5)
	durationS := envInt("CAPSID_BENCH_DURATION_S", 10)
	connections := envInt("CAPSID_BENCH_CONNECTIONS", 16)
	inflight := envInt("CAPSID_BENCH_INFLIGHT", 64)
	samplesOut := envOr("CAPSID_BENCH_SAMPLES_OUT", "")
	correctnessOut := envOr("CAPSID_BENCH_CORRECTNESS_OUT", "")
	if samplesOut == "" || correctnessOut == "" {
		fatal("CAPSID_BENCH_SAMPLES_OUT and CAPSID_BENCH_CORRECTNESS_OUT are required")
	}

	path := "/@capsid/orders/fixed"
	expectedLen := 1024
	expectedByte := byte(0x78)
	if workload == "cpu-template" {
		path = "/@capsid/orders/cpu"
		expectedLen = -1
	} else if workload != "fixed-1k" {
		fatal("unknown workload: " + workload)
	}

	// Fixed explicit headers for every request: the baseline and candidate
	// must forward the same end-to-end fields so the worker sees identical
	// input. The fixture's /@capsid/orders/inspect returns the actual
	// worker-visible headers, and the benchmark runner can cross-check them.
	fixedHeaders := map[string]string{
		"user-agent":      "capsid-loadgen/1.0",
		"accept":          "application/octet-stream",
		"x-trace":         "benchmark-fixed",
		"accept-encoding": "identity",
		"cache-control":   "no-cache",
	}

	client := &http.Client{
		Timeout: 30 * time.Second,
		Transport: &http.Transport{
			MaxIdleConnsPerHost: connections,
			MaxConnsPerHost:     connections,
			MaxIdleConns:        connections,
		},
	}

	var (
		mu        sync.Mutex
		latencies []float64 // ms
		lagTotal  time.Duration
		completed int
		errors    int
		timeouts  int
		mismatch  int
	)

	verify := func(body []byte) bool {
		if expectedLen >= 0 {
			if len(body) != expectedLen {
				return false
			}
			for _, b := range body {
				if b != expectedByte {
					return false
				}
			}
			return true
		}
		return len(body) > 0 && contains(body, "</ul>")
	}

	runPhase := func(phase string, seconds int) {
		start := time.Now()
		end := start.Add(time.Duration(seconds) * time.Second)
		sem := make(chan struct{}, inflight)
		var wg sync.WaitGroup
		phaseLatencies := []float64{}
		phaseLag := time.Duration(0)
		phaseCompleted, phaseErrors, phaseTimeouts, phaseMismatch := 0, 0, 0, 0

		// The dispatch loop saturates: every slot in flight is replaced as
		// soon as it completes, so the measured rate is the system's own.
		dispatch := func(createdAt time.Time) {
			defer wg.Done()
			defer func() { <-sem }()
			startedAt := time.Now()
			mu.Lock()
			lag := startedAt.Sub(createdAt)
			phaseLag += lag
			mu.Unlock()
			req, err := http.NewRequest("GET", target+path, nil)
			if err != nil {
				mu.Lock()
				phaseErrors++
				mu.Unlock()
				return
			}
			for k, v := range fixedHeaders {
				req.Header.Set(k, v)
			}
			req.Header.Set("connection", "keep-alive")
			req.Header.Set("host", "client-controlled.example")
			response, err := client.Do(req)
			if err != nil {
				mu.Lock()
				phaseTimeouts++
				mu.Unlock()
				return
			}
			body, readErr := io.ReadAll(response.Body)
			response.Body.Close()
			if readErr != nil {
				mu.Lock()
				phaseErrors++
				mu.Unlock()
				return
			}
			elapsed := float64(time.Since(startedAt)) / float64(time.Millisecond)
			mu.Lock()
			phaseLatencies = append(phaseLatencies, elapsed)
			if response.StatusCode != 200 || !verify(body) {
				phaseMismatch++
				phaseErrors++
			} else {
				phaseCompleted++
			}
			mu.Unlock()
		}

		for time.Now().Before(end) {
			createdAt := time.Now()
			sem <- struct{}{}
			wg.Add(1)
			go dispatch(createdAt)
		}
		wg.Wait()

		mu.Lock()
		defer mu.Unlock()
		latencies = append(latencies, phaseLatencies...)
		lagTotal += phaseLag
		completed += phaseCompleted
		errors += phaseErrors
		timeouts += phaseTimeouts
		mismatch += phaseMismatch

		measured := time.Since(start).Seconds()
		qps := 0.0
		if measured > 0 {
			qps = float64(phaseCompleted) / measured
		}
		p50, p95, p99 := percentile(phaseLatencies, 50), percentile(phaseLatencies, 95), percentile(phaseLatencies, 99)
		lagMs := 0.0
		if len(phaseLatencies) > 0 {
			lagMs = float64(phaseLag) / float64(time.Millisecond) / float64(len(phaseLatencies))
		}
		writeJSON(samplesOut, sample{
			Side: side, Round: round, Phase: phase,
			QPS: qps, P50Ms: p50, P95Ms: p95, P99Ms: p99,
			DispatchWaitMs: lagMs,
			Completed: phaseCompleted, Errors: phaseErrors, Timeouts: phaseTimeouts,
			DurationS: measured, Connections: connections, Inflight: inflight,
		})
	}

	runPhase("warmup", warmupS)
	runPhase("measured", durationS)

	verdict := correctness{
		OK:               errors == 0 && mismatch == 0,
		ResponsesChecked: completed + errors,
		Mismatches:       mismatch,
		Errors:           errors,
		Timeouts:         timeouts,
		ErrorsAsSuccess:  false,
	}
	if !verdict.OK {
		verdict.Error = fmt.Sprintf("%d of %d responses failed validation",
			errors, verdict.ResponsesChecked)
	}
	writeJSON(correctnessOut, verdict)
}

func percentile(values []float64, p int) float64 {
	if len(values) == 0 {
		return 0
	}
	sorted := append([]float64(nil), values...)
	sort.Float64s(sorted)
	index := (len(sorted)-1)*p/100
	return sorted[index]
}

func contains(haystack []byte, needle string) bool {
	return bytes.Contains(haystack, []byte(needle))
}

func writeJSON(path string, value any) {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		fatal("cannot open " + path + ": " + err.Error())
	}
	defer file.Close()
	writer := bufio.NewWriter(file)
	encoded, err := json.Marshal(value)
	if err != nil {
		fatal("cannot encode output: " + err.Error())
	}
	writer.Write(encoded)
	writer.WriteByte('\n')
	if err := writer.Flush(); err != nil {
		fatal("cannot write " + path + ": " + err.Error())
	}
}

func fatal(message string) {
	fmt.Fprintln(os.Stderr, "loadgen:", message)
	os.Exit(1)
}
