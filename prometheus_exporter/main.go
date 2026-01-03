package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strings"

	_ "github.com/lib/pq"
)

var whitespaceRegex = regexp.MustCompile(` +`)

type Config struct {
	DatabaseURL string
	Port        string
}

type Metric struct {
	Name      string
	Labels    map[string]interface{}
	Type      string
	Bucket    int
	Value     int64
	QueryText sql.NullString
}

func loadConfig() (*Config, error) {
	dbURL := os.Getenv("DATABASE_URL")
	if dbURL == "" {
		return nil, fmt.Errorf("DATABASE_URL environment variable is required")
	}

	port := os.Getenv("PORT")
	if port == "" {
		port = "9187"
	}

	return &Config{
		DatabaseURL: dbURL,
		Port:        port,
	}, nil
}

// compactQuery normalizes SQL query text for use in Prometheus labels
func compactQuery(query string) string {
	query = strings.ReplaceAll(query, "\n", " ")
	query = strings.ReplaceAll(query, "\r", " ")
	query = strings.ReplaceAll(query, "\t", " ")
	query = whitespaceRegex.ReplaceAllString(query, " ")
	return strings.TrimSpace(query)
}

// escapeLabelValue escapes values for Prometheus text format per the spec:
// https://prometheus.io/docs/instrumenting/exposition_formats/
func escapeLabelValue(value string) string {
	value = strings.ReplaceAll(value, "\\", "\\\\")
	value = strings.ReplaceAll(value, "\"", "\\\"")
	value = strings.ReplaceAll(value, "\n", "\\n")
	return value
}

// formatLabels converts label map to Prometheus label string format: {key1="val1",key2="val2"}
// Keys are sorted for deterministic output.
func formatLabels(labels map[string]interface{}) string {
	if len(labels) == 0 {
		return ""
	}

	keys := make([]string, 0, len(labels))
	for k := range labels {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	pairs := make([]string, 0, len(labels))
	for _, key := range keys {
		value := fmt.Sprintf("%v", labels[key])
		escaped := escapeLabelValue(value)
		pairs = append(pairs, fmt.Sprintf(`%s="%s"`, key, escaped))
	}

	return "{" + strings.Join(pairs, ",") + "}"
}

func fetchMetrics(db *sql.DB) ([]Metric, []int, error) {
	bucketRows, err := db.Query("SELECT bucket FROM pmetrics.list_histogram_buckets()")
	if err != nil {
		return nil, nil, fmt.Errorf("failed to fetch histogram buckets: %w", err)
	}
	defer bucketRows.Close()

	var allBuckets []int
	for bucketRows.Next() {
		var bucket int
		if err := bucketRows.Scan(&bucket); err != nil {
			return nil, nil, fmt.Errorf("failed to scan bucket: %w", err)
		}
		allBuckets = append(allBuckets, bucket)
	}
	if err := bucketRows.Err(); err != nil {
		return nil, nil, fmt.Errorf("error iterating buckets: %w", err)
	}
	sort.Ints(allBuckets)

	query := `
		SELECT
			m.name,
			m.labels,
			m.type,
			m.bucket,
			m.value,
			q.query_text
		FROM pmetrics.list_metrics() m
		LEFT JOIN pmetrics_stmts.list_queries() q
			ON (m.labels->>'queryid')::bigint = q.queryid
		ORDER BY m.name, m.type, m.labels::text, m.bucket
	`

	rows, err := db.Query(query)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to fetch metrics: %w", err)
	}
	defer rows.Close()

	var metrics []Metric
	for rows.Next() {
		var m Metric
		var labelsJSON []byte

		if err := rows.Scan(&m.Name, &labelsJSON, &m.Type, &m.Bucket, &m.Value, &m.QueryText); err != nil {
			return nil, nil, fmt.Errorf("failed to scan metric: %w", err)
		}

		if len(labelsJSON) > 0 {
			// Use decoder with UseNumber to preserve integer precision
			decoder := json.NewDecoder(strings.NewReader(string(labelsJSON)))
			decoder.UseNumber()
			if err := decoder.Decode(&m.Labels); err != nil {
				return nil, nil, fmt.Errorf("failed to parse labels: %w", err)
			}
		} else {
			m.Labels = make(map[string]interface{})
		}

		if m.QueryText.Valid && m.QueryText.String != "" {
			compacted := compactQuery(m.QueryText.String)
			// Truncate to fit Prometheus label size limits
			if len(compacted) > 200 {
				compacted = compacted[:200]
			}
			m.Labels["query"] = compacted
		}

		metrics = append(metrics, m)
	}

	if err := rows.Err(); err != nil {
		return nil, nil, fmt.Errorf("error iterating metrics: %w", err)
	}

	return metrics, allBuckets, nil
}

// formatMetrics converts pmetrics data to Prometheus text exposition format.
// Histograms are converted to cumulative buckets as required by Prometheus spec.
func formatMetrics(metrics []Metric, allBuckets []int) string {
	var lines []string
	emittedTypes := make(map[string]bool)

	histograms := make(map[string]map[int]int64)
	histogramSums := make(map[string]int64)
	histogramLabels := make(map[string]map[string]interface{})
	var simpleMetrics []Metric

	for _, m := range metrics {
		if m.Type == "histogram" {
			labelsJSON, err := json.Marshal(m.Labels)
			if err != nil {
				continue
			}
			key := m.Name + string(labelsJSON)

			if histograms[key] == nil {
				histograms[key] = make(map[int]int64)
				histogramLabels[key] = m.Labels
			}
			histograms[key][m.Bucket] = m.Value
		} else if m.Type == "histogram_sum" {
			labelsJSON, err := json.Marshal(m.Labels)
			if err != nil {
				continue
			}
			key := m.Name + string(labelsJSON)
			histogramSums[key] = m.Value
			histogramLabels[key] = m.Labels
		} else {
			simpleMetrics = append(simpleMetrics, m)
		}
	}

	for _, m := range simpleMetrics {
		if !emittedTypes[m.Name] {
			lines = append(lines, fmt.Sprintf("# TYPE %s %s", m.Name, m.Type))
			emittedTypes[m.Name] = true
		}

		labelStr := formatLabels(m.Labels)
		lines = append(lines, fmt.Sprintf("%s%s %d", m.Name, labelStr, m.Value))
	}

	histogramKeys := make([]string, 0, len(histograms))
	for key := range histograms {
		histogramKeys = append(histogramKeys, key)
	}
	sort.Strings(histogramKeys)

	for _, key := range histogramKeys {
		bucketValues := histograms[key]
		labels := histogramLabels[key]

		labelsJSON, err := json.Marshal(labels)
		if err != nil {
			continue
		}
		name := strings.TrimSuffix(key, string(labelsJSON))

		if !emittedTypes[name] {
			lines = append(lines, fmt.Sprintf("# TYPE %s histogram", name))
			emittedTypes[name] = true
		}

		baseLabelStr := formatLabels(labels)

		var cumulativeCount int64
		for _, bucketThreshold := range allBuckets {
			bucketValue := bucketValues[bucketThreshold]
			cumulativeCount += bucketValue

			var labelStr string
			if baseLabelStr != "" {
				labelStr = baseLabelStr[:len(baseLabelStr)-1] + fmt.Sprintf(`,le="%d"`, bucketThreshold) + "}"
			} else {
				labelStr = fmt.Sprintf(`{le="%d"}`, bucketThreshold)
			}

			lines = append(lines, fmt.Sprintf("%s_bucket%s %d", name, labelStr, cumulativeCount))
		}

		var infLabelStr string
		if baseLabelStr != "" {
			infLabelStr = baseLabelStr[:len(baseLabelStr)-1] + `,le="+Inf"}`
		} else {
			infLabelStr = `{le="+Inf"}`
		}
		lines = append(lines, fmt.Sprintf("%s_bucket%s %d", name, infLabelStr, cumulativeCount))

		lines = append(lines, fmt.Sprintf("%s_count%s %d", name, baseLabelStr, cumulativeCount))

		sumValue := histogramSums[key]
		lines = append(lines, fmt.Sprintf("%s_sum%s %d", name, baseLabelStr, sumValue))
	}

	return strings.Join(lines, "\n") + "\n"
}

func metricsHandler(db *sql.DB) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		metrics, allBuckets, err := fetchMetrics(db)
		if err != nil {
			log.Printf("Error fetching metrics: %v", err)
			http.Error(w, fmt.Sprintf("Error: %v", err), http.StatusInternalServerError)
			return
		}

		output := formatMetrics(metrics, allBuckets)

		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		io.WriteString(w, output)
	}
}

func main() {
	config, err := loadConfig()
	if err != nil {
		log.Fatalf("Configuration error: %v", err)
	}

	db, err := sql.Open("postgres", config.DatabaseURL)
	if err != nil {
		log.Fatalf("Failed to connect to database: %v", err)
	}
	defer db.Close()

	if err := db.Ping(); err != nil {
		log.Fatalf("Failed to ping database: %v", err)
	}

	http.HandleFunc("/metrics", metricsHandler(db))

	addr := ":" + config.Port
	log.Printf("pmetrics Prometheus exporter listening on %s", addr)
	log.Printf("Metrics available at http://localhost:%s/metrics", config.Port)

	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("Server failed: %v", err)
	}
}
