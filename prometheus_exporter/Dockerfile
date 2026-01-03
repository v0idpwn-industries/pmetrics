# Multi-stage build for minimal final image
FROM golang:1.21-alpine AS builder

WORKDIR /build

# Copy go mod files
COPY go.mod go.sum* ./

# Download dependencies
RUN go mod download

# Copy source code
COPY *.go ./

# Build static binary
RUN CGO_ENABLED=0 GOOS=linux go build -a -installsuffix cgo -ldflags '-s -w' -o pmetrics-exporter .

# Final stage - minimal image
FROM scratch

# Copy SSL certificates for HTTPS connections
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/

# Copy the binary
COPY --from=builder /build/pmetrics-exporter /pmetrics-exporter

# Expose default port
EXPOSE 9187

# Run the exporter
ENTRYPOINT ["/pmetrics-exporter"]
