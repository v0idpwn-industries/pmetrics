.PHONY: build clean docker-build

BINARY_NAME=pmetrics-exporter

# Build the binary
build:
	go build -o $(BINARY_NAME) .

# Clean build artifacts
clean:
	rm -f $(BINARY_NAME)

# Build Docker image
docker-build:
	docker build -t pmetrics-exporter:latest .
