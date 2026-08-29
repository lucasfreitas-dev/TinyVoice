FROM golang:1.22-alpine AS builder

RUN apk add --no-cache git ca-certificates

WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /tinyvoice-api ./cmd/server
RUN CGO_ENABLED=0 GOOS=linux go build -o /tinyvoice-cli ./cmd/tinyvoice

FROM alpine:3.20

RUN apk add --no-cache ca-certificates ffmpeg

COPY --from=builder /tinyvoice-api /usr/local/bin/tinyvoice-api
COPY --from=builder /tinyvoice-cli /usr/local/bin/tinyvoice
COPY migrations /migrations

ENV MIGRATIONS_PATH=/migrations
EXPOSE 8080

CMD ["tinyvoice-api"]
