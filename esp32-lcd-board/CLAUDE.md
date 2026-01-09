# Project Instructions

## TfNSW API Documentation

### Server Details
- **Base URL**: `https://api.transport.nsw.gov.au/v1/`
- **Protocol**: HTTPS on port 443
- **API Version**: v1 (included in URL path)

### Authentication
API requests require an `Authorization` header with your API key:

```
Authorization: apikey <your-api-key>
```

**Error Response (401 Unauthorized):**
```json
{
  "ErrorDetails": {
    "TransactionId": "00000153065eb1f7-38f0e",
    "ErrorDateTime": "2016-03-19T04:35:02.676-04:00",
    "Message": "The calling application is unauthenticated.",
    "RequestedUrl": "/v1/gtfs/alerts/buses",
    "RequestMethod": "GET"
  }
}
```

### Rate Limits (Bronze Plan - Default)
- **Quota**: 60,000 requests per day
- **Rate Limit**: 5 requests per second

### Rate Limit Errors

**Throttle Limit Exceeded (403):**
- Header: `X-Error-Detail: Account Over Rate Limit`
```json
{
  "code": 403,
  "message": "Account over rate limit"
}
```

**Quota Limit Exceeded (403):**
- Header: `X-Error-Detail: Account Over Quota Limit`
```json
{
  "code": 403,
  "message": "Account over rate limit"
}
```

### Technical Notes
- All endpoints use HTTP GET requests with query parameters
- Responses are typically JSON encoded
- Keep API keys private - avoid exposing in URLs or markup
- For AJAX searches, proxy requests through your own server

---

## GTFS Realtime API

GTFS-Realtime provides real-time transit information including trip updates, vehicle positions, and service alerts. The TfNSW implementation uses Protocol Buffer (protobuf) format.

### Feed Types

| Feed Type | Description | Update Frequency |
|-----------|-------------|------------------|
| Trip Updates | Early/delays, cancellations, added trips, changed routes | Every 20 seconds (hysteresis) |
| Vehicle Positions | Location, occupancy, congestion level | Every 15 seconds |
| Service Alerts | Disruptions, cancellations, long delays | Auto-generated + manual |

### Time Windows
- **Trip Updates**: Start 60 minutes before trip, continue 30 minutes after completion
- **Vehicle Positions**: Start when status is "atOrigin", end when "completed"
- **Feed Capacity**: Trips in progress + up to 30 min past + next 60 min future

### API Endpoints (GTFS-R)

```
GET /v2/gtfs/realtime/tripupdates/{mode}
GET /v2/gtfs/realtime/vehiclepositions/{mode}
GET /v2/gtfs/realtime/alerts/{mode}
```

**Modes**: `sydneytrains`, `buses`, `ferries`, `lightrail`, `metro`, `nswtrains`, `regionbuses`

---

## Trip Updates Message Structure

### Message Format (Protobuf)
```protobuf
entity {
  id: "unique-entity-id"
  trip_update {
    trip {
      trip_id: "M-I-CUD-CHW-1-1501-3116:1000"
      route_id: "SMNW_M"
      direction_id: 1
      start_time: "15:01:00"
      start_date: "20230720"
      schedule_relationship: SCHEDULED
    }
    vehicle {
      id: "RS001"
      label: "RS001"
    }
    stop_time_update {
      stop_sequence: 1
      stop_id: "2155270"
      departure {
        delay: 0
        time: 1689829260
      }
      schedule_relationship: SCHEDULED
    }
    timestamp: 1689825635
  }
}
```

### Trip Descriptor Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `trip_id` | string | Yes | Unique trip identifier from GTFS, must be unique within feed |
| `route_id` | string | Yes | Route identifier from GTFS |
| `direction_id` | uint32 | Conditional | 0 or 1, required for ADDED trips |
| `start_time` | string | Yes | Trip start time (HH:MM:SS format) |
| `start_date` | string | Yes | Trip date (YYYYMMDD format) |
| `schedule_relationship` | enum | Yes | Trip status (see below) |

### Schedule Relationship (Trip Level)

| Value | Description | TfNSW Rules |
|-------|-------------|-------------|
| `SCHEDULED` | Running per GTFS schedule | trip_id must exist and be unique in GTFS |
| `ADDED` | Extra trip added to schedule | trip_id must NOT exist in GTFS, must have full stop sequence, direction_id required |
| `UNSCHEDULED` | Running without schedule | No trip_id, has route_id, only future stops have real-time info |
| `CANCELED` | Trip removed from schedule | trip_id must be SCHEDULED or ADDED trip |
| `REPLACEMENT` | Replacement service | Full stop sequence required including skipped stops |

### StopTimeUpdate Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `stop_sequence` | uint32 | Yes | Stop order (from GTFS stop_times.txt) |
| `stop_id` | string | Yes | Stop identifier (from GTFS stops.txt) |
| `arrival` | StopTimeEvent | Conditional | Arrival time info |
| `departure` | StopTimeEvent | Conditional | Departure time info |
| `schedule_relationship` | enum | Optional | Stop-level status |

### StopTimeEvent Fields

| Field | Type | Description |
|-------|------|-------------|
| `delay` | int32 | Delay in seconds (+late, -early, 0=on time) |
| `time` | int64 | Absolute time (POSIX timestamp) |
| `uncertainty` | int32 | Expected error in seconds (0=certain) |

### Stop-Level Schedule Relationship

| Value | Description |
|-------|-------------|
| `SCHEDULED` | Normal stop, arrival/departure required |
| `SKIPPED` | Vehicle won't stop here |
| `NO_DATA` | No real-time info available |

---

## Vehicle Position Message Structure

### Message Format (Protobuf)
```protobuf
entity {
  id: "0/2023-07-20T05:02:37Z/RS001"
  vehicle {
    trip {
      trip_id: "M-I-CUD-CHW-2-1505-3128:1000"
      route_id: "SMNW_M"
      direction_id: 1
      start_time: "15:05:00"
      start_date: "20230720"
      schedule_relationship: SCHEDULED
    }
    vehicle {
      id: "RS001"
      label: "RS001"
      license_plate: "RS001"
      tfnsw_vehicle_descriptor {
        air_conditioned: true
        wheelchair_accessible: 1
        vehicle_model: "Alstom Metropolis"
        special_vehicle_attributes: 0
      }
    }
    position {
      latitude: -33.6913567
      longitude: 150.906723
      bearing: 70
      speed: 0
    }
    current_stop_sequence: 1
    stop_id: "2155269"
    current_status: STOPPED_AT
    timestamp: 1689829357
    congestion_level: SEVERE_CONGESTION
    occupancy_status: MANY_SEATS_AVAILABLE
    consist {
      name: "DTC1"
      position_in_consist: 0
      occupancy_status: MANY_SEATS_AVAILABLE
      quiet_carriage: false
      toilet: NONE
      luggage_rack: false
    }
  }
}
```

### Vehicle Position Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Traceable to source system |
| `trip` | TripDescriptor | Yes | Trip being served |
| `vehicle` | VehicleDescriptor | Yes | Vehicle information |
| `position` | Position | Yes | Current GPS location |
| `current_stop_sequence` | uint32 | Yes | Current stop index |
| `stop_id` | string | Yes | Current/next stop ID |
| `current_status` | VehicleStopStatus | Yes | Status relative to stop |
| `timestamp` | uint64 | Yes | POSIX timestamp of position |
| `congestion_level` | enum | Yes | Traffic congestion |
| `occupancy_status` | enum | Yes | Passenger load |
| `consist` | CarriageDescriptor[] | Yes | Carriage-level info (trains) |

### Position Fields

| Field | Type | Required | Precision |
|-------|------|----------|-----------|
| `latitude` | float | Yes | 6 decimal places |
| `longitude` | float | Yes | 6 decimal places |
| `bearing` | float | Yes | 2 decimal places (0=North, 90=East) |
| `speed` | float | Yes | 2 decimal places (m/s) |

### Vehicle Stop Status

| Value | Description |
|-------|-------------|
| `INCOMING_AT` | About to arrive (display flashes) |
| `STOPPED_AT` | Standing at stop |
| `IN_TRANSIT_TO` | Departed previous, en route (default) |

### Congestion Level

| Value | Formula |
|-------|---------|
| `UNKNOWN_CONGESTION_LEVEL` | Short-term average unavailable |
| `RUNNING_SMOOTHLY` | ẍLong + σLong > ẍshort |
| `STOP_AND_GO` | ẍLong + σLong <= ẍshort < ẍLong + 2σLong |
| `CONGESTION` | ẍLong + 2σLong <= ẍshort < ẍLong + 3σLong |
| `SEVERE_CONGESTION` | ẍLong + 3σLong <= ẍshort |

### Occupancy Status

| Value | TfNSW Rule |
|-------|------------|
| `MANY_SEATS_AVAILABLE` | Passengers ≤ 50% seating capacity |
| `FEW_SEATS_AVAILABLE` | Passengers ≤ seating capacity |
| `STANDING_ROOM_ONLY` | Passengers > seating capacity |
| `EMPTY` | Not populated |
| `FULL` | Not populated |

---

## TfNSW Extensions (Extension 1007)

### tfnsw_vehicle_descriptor

| Field | Type | Description |
|-------|------|-------------|
| `air_conditioned` | bool | A/C availability |
| `wheelchair_accessible` | int | 0=No, 1=Yes |
| `vehicle_model` | string | Bus: "Manufacturer~Chassis~BodyMfr~Body" |
| `performing_prior_trip` | bool | Vehicle still on previous trip |
| `special_vehicle_attributes` | int | Bitmask: 0001=WiFi, 0010=Christmas bus |

### CarriageDescriptor (Trains)

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Carriage name visible to passengers |
| `position_in_consist` | int | Position from leading carriage (1-based) |
| `occupancy_status` | enum | Carriage-specific occupancy |
| `quiet_carriage` | bool | Quiet carriage designation |
| `toilet` | enum | 0=NONE, 1=NORMAL, 2=ACCESSIBLE |
| `luggage_rack` | bool | Has luggage racks |

---

## Implementation Guidelines

### Parsing GTFS-R Feeds

1. **Use protobuf library** - Feeds are Protocol Buffer encoded, not JSON
2. **Handle full dataset** - Each response is `incrementality: FULL_DATASET`
3. **Check timestamps** - Use `header.timestamp` for feed freshness
4. **Match trip_ids** - Cross-reference with GTFS static data

### Calculating Real-Time Arrivals

```typescript
// For SCHEDULED trips:
const scheduledTime = getScheduledTimeFromGTFS(trip_id, stop_id);
const realTime = scheduledTime + stop_time_update.arrival.delay;

// Or use absolute time if provided:
const realTime = stop_time_update.arrival.time;
```

### Handling Metro/Light Rail

> **Important**: For high-frequency services (Metro, Light Rail), ignore delay information and show only real-time arrival/departure times. These services adjust to headway operationally.

### Error Handling

- **Missing trip in feed** = No real-time info (show scheduled time)
- **Duplicate trip_ids** = Trips reset to scheduled (data ignored)
- **NO_DATA schedule_relationship** = No real-time for entire trip

### Polling Strategy

- Poll Trip Updates: Every 20-30 seconds
- Poll Vehicle Positions: Every 15-20 seconds
- Respect rate limits: 5 requests/second max

### Data Freshness

- Check `entity.trip_update.timestamp` for last vehicle update
- Stale data (>2 min old) may indicate vehicle tracking issues
- `timestamp` in header = feed generation time

---

## TypeScript Type Definitions

```typescript
interface TripUpdate {
  trip: TripDescriptor;
  vehicle?: VehicleDescriptor;
  stop_time_update: StopTimeUpdate[];
  timestamp?: number;
}

interface TripDescriptor {
  trip_id: string;
  route_id: string;
  direction_id?: 0 | 1;
  start_time: string;  // "HH:MM:SS"
  start_date: string;  // "YYYYMMDD"
  schedule_relationship: ScheduleRelationship;
}

type ScheduleRelationship =
  | 'SCHEDULED'
  | 'ADDED'
  | 'UNSCHEDULED'
  | 'CANCELED'
  | 'REPLACEMENT';

interface StopTimeUpdate {
  stop_sequence: number;
  stop_id: string;
  arrival?: StopTimeEvent;
  departure?: StopTimeEvent;
  schedule_relationship?: StopScheduleRelationship;
}

type StopScheduleRelationship = 'SCHEDULED' | 'SKIPPED' | 'NO_DATA';

interface StopTimeEvent {
  delay?: number;      // seconds
  time?: number;       // POSIX timestamp
  uncertainty?: number; // seconds
}

interface VehiclePosition {
  trip: TripDescriptor;
  vehicle: VehicleDescriptor;
  position: Position;
  current_stop_sequence: number;
  stop_id: string;
  current_status: VehicleStopStatus;
  timestamp: number;
  congestion_level: CongestionLevel;
  occupancy_status: OccupancyStatus;
  consist?: CarriageDescriptor[];
}

type VehicleStopStatus = 'INCOMING_AT' | 'STOPPED_AT' | 'IN_TRANSIT_TO';

type CongestionLevel =
  | 'UNKNOWN_CONGESTION_LEVEL'
  | 'RUNNING_SMOOTHLY'
  | 'STOP_AND_GO'
  | 'CONGESTION'
  | 'SEVERE_CONGESTION';

type OccupancyStatus =
  | 'EMPTY'
  | 'MANY_SEATS_AVAILABLE'
  | 'FEW_SEATS_AVAILABLE'
  | 'STANDING_ROOM_ONLY'
  | 'CRUSHED_STANDING_ROOM_ONLY'
  | 'FULL'
  | 'NOT_ACCEPTING_PASSENGERS';

interface Position {
  latitude: number;   // WGS-84, 6 decimals
  longitude: number;  // WGS-84, 6 decimals
  bearing?: number;   // degrees from North
  speed?: number;     // m/s
}

interface TfNSWVehicleDescriptor {
  air_conditioned?: boolean;
  wheelchair_accessible?: 0 | 1;
  vehicle_model?: string;
  performing_prior_trip?: boolean;
  special_vehicle_attributes?: number; // bitmask
}

interface CarriageDescriptor {
  name?: string;
  position_in_consist: number;
  occupancy_status: OccupancyStatus;
  quiet_carriage?: boolean;
  toilet?: 0 | 1 | 2; // NONE, NORMAL, ACCESSIBLE
  luggage_rack?: boolean;
}
```
