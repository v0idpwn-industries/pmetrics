#!/usr/bin/env python3
"""
Simple Prometheus exporter for pmetrics.
Queries PostgreSQL and exposes metrics on :9187/metrics
"""
import os
import json
from http.server import HTTPServer, BaseHTTPRequestHandler
import psycopg2
from psycopg2.extras import RealDictCursor


def escape_label_value(value):
    """Escape special characters in Prometheus label values."""
    value = str(value)
    value = value.replace('\\', '\\\\')
    value = value.replace('"', '\\"')
    value = value.replace('\n', '\\n')
    return value


def format_labels(labels_dict):
    """Format a dict of labels into Prometheus label string."""
    if not labels_dict:
        return ""

    pairs = []
    for key, value in sorted(labels_dict.items()):
        escaped = escape_label_value(value)
        pairs.append(f'{key}="{escaped}"')

    return '{' + ','.join(pairs) + '}'


def get_metrics():
    """Fetch metrics from PostgreSQL and format as Prometheus text."""
    # Get connection string from environment (required)
    conn_string = os.environ.get('DATABASE_URL')
    if not conn_string:
        raise ValueError('DATABASE_URL environment variable is required')

    with psycopg2.connect(conn_string) as conn:
        with conn.cursor(cursor_factory=RealDictCursor) as cur:
            # Get all possible histogram buckets
            cur.execute("SELECT bucket FROM pmetrics.list_histogram_buckets()")
            all_buckets = sorted([row['bucket'] for row in cur.fetchall()])

            # Join metrics with queries in PostgreSQL
            cur.execute("""
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
            """)
            metrics = cur.fetchall()

    # Group metrics for processing
    from collections import defaultdict
    histograms = defaultdict(lambda: defaultdict(int))  # {(name, labels_json): {bucket: value}}
    histogram_sums = {}  # {(name, labels_json): sum_value}
    simple_metrics = []  # counters and gauges

    for metric in metrics:
        name = metric['name']
        metric_type = metric['type']
        labels = dict(metric['labels']) if metric['labels'] else {}
        bucket = metric['bucket']
        value = metric['value']
        query_text = metric['query_text']

        # Add query text to labels if available
        if query_text:
            labels['query'] = query_text[:200]

        if metric_type == 'histogram':
            # Group histogram buckets by (name, labels)
            labels_key = json.dumps(labels, sort_keys=True)
            histograms[(name, labels_key)][bucket] = value
        elif metric_type == 'histogram_sum':
            # Store histogram sum
            labels_key = json.dumps(labels, sort_keys=True)
            histogram_sums[(name, labels_key)] = value
        else:
            simple_metrics.append((name, metric_type, labels, value))

    lines = []
    emitted_types = set()

    # Output simple metrics (counters and gauges)
    for name, metric_type, labels, value in simple_metrics:
        if name not in emitted_types:
            lines.append(f'# TYPE {name} {metric_type}')
            emitted_types.add(name)

        label_str = format_labels(labels)
        lines.append(f'{name}{label_str} {value}')

    # Output histograms with cumulative buckets
    for (name, labels_json), bucket_values in sorted(histograms.items()):
        if name not in emitted_types:
            lines.append(f'# TYPE {name} histogram')
            emitted_types.add(name)

        labels = json.loads(labels_json)
        base_label_str = format_labels(labels)

        # Use all possible buckets, filling in zeros for missing ones
        cumulative_count = 0

        for bucket_threshold in all_buckets:
            # Get value for this bucket (0 if not recorded)
            bucket_value = bucket_values.get(bucket_threshold, 0)
            cumulative_count += bucket_value

            # Format bucket line with le label
            if base_label_str:
                label_str = base_label_str[:-1] + f',le="{bucket_threshold}"' + '}'
            else:
                label_str = '{le="' + str(bucket_threshold) + '"}'

            lines.append(f'{name}_bucket{label_str} {cumulative_count}')

        # Add +Inf bucket (required by spec)
        if base_label_str:
            inf_label_str = base_label_str[:-1] + ',le="+Inf"}'
        else:
            inf_label_str = '{le="+Inf"}'
        lines.append(f'{name}_bucket{inf_label_str} {cumulative_count}')

        # Add _count metric (equal to +Inf bucket)
        lines.append(f'{name}_count{base_label_str} {cumulative_count}')

        # Add _sum metric
        sum_value = histogram_sums.get((name, labels_json), 0)
        lines.append(f'{name}_sum{base_label_str} {sum_value}')

    return '\n'.join(lines) + '\n'


class MetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/metrics':
            try:
                metrics = get_metrics()
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain; version=0.0.4')
                self.end_headers()
                self.wfile.write(metrics.encode('utf-8'))
            except Exception as e:
                self.send_response(500)
                self.send_header('Content-Type', 'text/plain')
                self.end_headers()
                self.wfile.write(f'Error: {str(e)}\n'.encode('utf-8'))
        elif self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            html = '<html><body><h1>pmetrics Prometheus Exporter</h1><p><a href="/metrics">Metrics</a></p></body></html>'
            self.wfile.write(html.encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        # Simple logging
        print(f"{self.address_string()} - {format % args}")


def main():
    port = int(os.environ.get('PORT', 9187))
    server = HTTPServer(('', port), MetricsHandler)
    print(f'pmetrics Prometheus exporter listening on :{port}')
    print(f'Metrics available at http://localhost:{port}/metrics')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nShutting down...')
        server.shutdown()


if __name__ == '__main__':
    main()
