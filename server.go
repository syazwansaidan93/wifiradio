package main

import (
        "encoding/json"
        "fmt"
        "io"
        "log"
        "net"
        "net/http"
        "os"
        "os/exec"
        "os/signal"
        "strconv"
        "sync"
        "syscall"
)

const configPath = "config.json"
const statePath = "state.txt" // File to persist the current station index

type Station struct {
        Name        string `json:"name"`
        StreamURL   string `json:"streamURL"`
        MetadataURL string `json:"metadataURL"`
}

type Config struct {
        Stations []Station `json:"stations"`
}

type SongInfo struct {
        Timestamp string `json:"timestamp"`
        Artist    string `json:"artist"`
        Title     string `json:"title"`
}

type PlaylistResponse struct {
        NowPlaying []SongInfo `json:"nowplaying"`
}

type NowPlayingResponse struct {
        Artist string `json:"artist"`
        Title  string `json:"title"`
}

type ServerState struct {
        config      *Config
        currentIdx  int
        stations    []Station
        currentConn net.Conn
        mu          sync.RWMutex
}

var serverState *ServerState

// saveCurrentIndex persists the current station index to a file.
func (s *ServerState) saveCurrentIndex() {
        s.mu.RLock()
        index := s.currentIdx
        s.mu.RUnlock()

        data := []byte(strconv.Itoa(index))
        if err := os.WriteFile(statePath, data, 0644); err != nil {
                log.Printf("ERROR: Failed to save current station index to %s: %v", statePath, err)
        }
}

// loadCurrentIndex loads the current station index from a file, defaulting to 0 on failure.
func loadCurrentIndex(maxStations int) int {
        data, err := os.ReadFile(statePath)
        if err != nil {
                log.Printf("INFO: Could not read persistent state file %s, starting from index 0. Error: %v", statePath, err)
                return 0
        }

        index, err := strconv.Atoi(string(data))
        if err != nil || index < 0 || index >= maxStations {
                log.Printf("WARNING: Invalid index found in %s (%s). Starting from index 0.", statePath, string(data))
                return 0
        }

        log.Printf("INFO: Loaded current station index: %d", index)
        return index
}

func loadConfig(path string) (*Config, error) {
        file, err := os.Open(path)
        if err != nil {
                return nil, fmt.Errorf("failed to open config file: %w", err)
        }
        defer file.Close()

        var config Config
        if err := json.NewDecoder(file).Decode(&config); err != nil {
                return nil, fmt.Errorf("failed to decode config file: %w", err)
        }

        if len(config.Stations) == 0 {
                return nil, fmt.Errorf("no stations defined in config")
        }

        return &config, nil
}

func fetchMetadata(url string) (SongInfo, error) {

        req, err := http.NewRequest("GET", url, nil)
        if err != nil {
                return SongInfo{}, fmt.Errorf("error creating request: %w", err)
        }

        req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36")

        resp, err := http.DefaultClient.Do(req)
        if err != nil {
                return SongInfo{}, fmt.Errorf("error fetching metadata: %w", err)
        }
        defer resp.Body.Close()

        if resp.StatusCode != http.StatusOK {
                return SongInfo{}, fmt.Errorf("received non-200 status code: %d", resp.StatusCode)
        }

        var data PlaylistResponse
        if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
                return SongInfo{}, fmt.Errorf("error decoding metadata JSON: %w", err)
        }

        if len(data.NowPlaying) > 0 {
                song := data.NowPlaying[0]
                return song, nil
        }
        return SongInfo{Artist: "Unknown", Title: "Unknown", Timestamp: "N/A"}, fmt.Errorf("now playing array is empty")
}

func (s *ServerState) getCurrentStation() Station {
        s.mu.RLock()
        defer s.mu.RUnlock()
        if len(s.stations) == 0 {
                return Station{StreamURL: "", MetadataURL: ""}
        }
        idx := s.currentIdx % len(s.stations)
        return s.stations[idx]
}

func createNowPlayingHandler(s *ServerState) http.HandlerFunc {
        return func(w http.ResponseWriter, r *http.Request) {
                current := s.getCurrentStation()
                song, err := fetchMetadata(current.MetadataURL)

                if err != nil {
                        log.Printf("Failed to fetch metadata on demand: %v", err)

                        w.Header().Set("Content-Type", "text/plain")
                        w.WriteHeader(http.StatusInternalServerError)
                        w.Write([]byte("Error - Error fetching song data"))
                        return
                }

                responsePayload := NowPlayingResponse{
                        Artist: song.Artist,
                        Title:  song.Title,
                }

                jsonResponse, err := json.Marshal(responsePayload)
                if err != nil {
                        log.Printf("Failed to marshal JSON response: %v", err)
                        w.Header().Set("Content-Type", "text/plain")
                        w.WriteHeader(http.StatusInternalServerError)
                        w.Write([]byte("Error - Failed to format JSON response"))
                        return
                }

                w.Header().Set("Content-Type", "application/json")
                w.WriteHeader(http.StatusOK)
                w.Write(jsonResponse)
        }
}

func createNextHandler(s *ServerState) http.HandlerFunc {
        return func(w http.ResponseWriter, r *http.Request) {
                s.mu.Lock()
                if s.currentConn != nil {
                        log.Printf("Closing active connection to switch station")
                        s.currentConn.Close()
                        s.currentConn = nil
                }
                s.currentIdx = (s.currentIdx + 1) % len(s.stations)
                currentName := s.stations[s.currentIdx].Name
                s.mu.Unlock()

                // Save the new index immediately
                s.saveCurrentIndex()

                log.Printf("Switched to next station: %s", currentName)

                w.Header().Set("Content-Type", "application/json")
                w.WriteHeader(http.StatusOK)
                response := map[string]interface{}{
                        "message": "Switched to next station",
                        "station": currentName,
                        "index":   s.currentIdx,
                }
                json.NewEncoder(w).Encode(response)
        }
}

func createCurrentHandler(s *ServerState) http.HandlerFunc {
        return func(w http.ResponseWriter, r *http.Request) {
                s.mu.RLock()
                currentName := s.stations[s.currentIdx].Name
                index := s.currentIdx
                s.mu.RUnlock()

                w.Header().Set("Content-Type", "application/json")
                w.WriteHeader(http.StatusOK)
                response := map[string]interface{}{
                        "station": currentName,
                        "index":   index,
                }
                json.NewEncoder(w).Encode(response)
        }
}

func createRestartHandler() http.HandlerFunc {
        return func(w http.ResponseWriter, r *http.Request) {
                log.Println("Received /restart request. Initiating graceful shutdown for external restart...")

                // Ensure state is saved before shutting down
                serverState.saveCurrentIndex()

                w.Header().Set("Content-Type", "application/json")
                w.WriteHeader(http.StatusOK)
                json.NewEncoder(w).Encode(map[string]string{"message": "Server restarting. Expect a brief disconnect."})

                p, err := os.FindProcess(os.Getpid())
                if err != nil {
                        log.Printf("Failed to find self process: %v. Exiting directly.", err)
                        os.Exit(0)
                        return
                }

                go func() {
                        p.Signal(syscall.SIGTERM)
                }()
        }
}

func startMetadataServer(s *ServerState) {
        metadataPort := ":8889"
        log.Println("Metadata service started on", metadataPort, "Query /nowplaying (on demand), /next to switch station, /current for current station, /restart to reboot the server")

        http.HandleFunc("/nowplaying", createNowPlayingHandler(s))
        http.HandleFunc("/next", createNextHandler(s))
        http.HandleFunc("/current", createCurrentHandler(s))
        http.HandleFunc("/restart", createRestartHandler())

        if err := http.ListenAndServe(metadataPort, nil); err != nil && err != http.ErrServerClosed {
                log.Fatalf("Failed to start metadata server: %v", err)
        }
}

func startStreamer(s *ServerState, listener net.Listener) {
        log.Println("Audio Streamer is listening on port 8888. Waiting for connection from ESP32...")

        for {
                conn, err := listener.Accept()
                if err != nil {
                        if opErr, ok := err.(*net.OpError); ok && opErr.Err.Error() == "use of closed network connection" {
                                return
                        }
                        log.Printf("Failed to accept connection: %v", err)
                        continue
                }

                s.mu.Lock()
                s.currentConn = conn
                s.mu.Unlock()

                current := s.getCurrentStation()
                ffmpegCmd := exec.Command("ffmpeg",
                        "-i", current.StreamURL,
                        "-acodec", "pcm_s16le",
                        "-ac", "2",
                        "-ar", "44100",
                        "-f", "s16le",
                        "pipe:1",
                )

                stdout, err := ffmpegCmd.StdoutPipe()
                if err != nil {
                        log.Printf("Failed to get stdout pipe: %v", err)
                        s.mu.Lock()
                        s.currentConn = nil
                        s.mu.Unlock()
                        conn.Close()
                        continue
                }

                if err := ffmpegCmd.Start(); err != nil {
                        log.Printf("Failed to start ffmpeg: %v", err)
                        stdout.Close()
                        s.mu.Lock()
                        s.currentConn = nil
                        s.mu.Unlock()
                        conn.Close()
                        continue
                }

                n, copyErr := io.Copy(conn, stdout)
                if copyErr != nil {
                        log.Printf("Connection broken after %d bytes: %v", n, copyErr)
                } else {
                        log.Printf("Stream ended normally after %d bytes", n)
                }

                if closeErr := stdout.Close(); closeErr != nil {
                        log.Printf("Failed to close stdout pipe: %v", closeErr)
                }

                if waitErr := ffmpegCmd.Wait(); waitErr != nil {
                        if exitErr, ok := waitErr.(*exec.ExitError); ok {
                                if exitErr.ExitCode() == 224 {
                                        log.Printf("ffmpeg exited with expected EPIPE (status %d)", exitErr.ExitCode())
                                } else {
                                        log.Printf("Unexpected ffmpeg exit code: %d", exitErr.ExitCode())
                                }
                        } else {
                                log.Printf("Error waiting for ffmpeg process: %v", waitErr)
                        }
                } else {
                        log.Printf("ffmpeg exited normally with status 0")
                }

                s.mu.Lock()
                s.currentConn = nil
                s.mu.Unlock()

                conn.Close()
        }
}

func main() {
        config, err := loadConfig(configPath)
        if err != nil {
                log.Fatalf("Failed to load configuration: %v", err)
        }

        initialIndex := loadCurrentIndex(len(config.Stations))

        serverState = &ServerState{
                config:     config,
                currentIdx: initialIndex,
                stations:   config.Stations,
        }
        log.Printf("Loaded %d stations. Starting with: %s (Index %d)", len(config.Stations), config.Stations[initialIndex].Name, initialIndex)

        sigchan := make(chan os.Signal, 1)
        signal.Notify(sigchan, syscall.SIGINT, syscall.SIGTERM)

        listener, err := net.Listen("tcp", ":8888")
        if err != nil {
                log.Fatalf("Failed to start TCP listener on :8888: %v", err)
        }

        go startMetadataServer(serverState)
        go startStreamer(serverState, listener)

        // Wait for the signal to shut down
        <-sigchan

        // Ensure the current index is saved before the application exits
        serverState.saveCurrentIndex()

        log.Println("Stopping the server.")
        listener.Close()
}