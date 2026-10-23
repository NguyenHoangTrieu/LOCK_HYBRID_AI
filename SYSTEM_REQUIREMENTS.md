# Software Requirements Specification
## Face and RFID Attendance System

**Version:** 2.2
**Status:** Draft for review
**Structure:** Based on IEEE 830 / ISO-IEC-IEEE 29148, adapted for an embedded system

### Revision History

| Version | Date | Summary |
|---|---|---|
| 2.0 | — | Baseline. |
| 2.1 | 2026-10-21 | (a) Server-managed nodes with two operating modes: **Attendance** and **Enrollment** (§3.2.8, §3.2.9). (b) Device UI changed from live camera preview to a lock-style clock screen with a captured-face confirmation screen (§3.1.1). (c) Card authorization moved to a local card store on the device, kept in sync with the server (§3.2.2, §3.2.10). (d) RFID cards are reusable after release or reassignment (§3.2.11). (e) Usability requirements extended for both the device and the admin interface (§3.9). Superseded: old FR-10, FR-11, FR-23, UI-1. |
| 2.2 | 2026-10-21 | Wi-Fi provisioning: pressing the provisioning button makes the ESP32-C5 open a temporary access point with a configuration web page, so the user can join the node to the same network as the server (§3.2.12, UI-26..UI-29, SR-12..SR-14, CON-11, OI-10). |

---

## 1. Introduction

### 1.1 Purpose

This document states what the Face and RFID Attendance System must do and how well it must do it. It is written for the engineers who will design and build the system, and for the stakeholders who need to confirm that the right product is being built.

This document describes **requirements only**. It does not describe how those requirements will be met. Design decisions — module structure, interfaces, algorithms, data formats — belong in the architecture and design documents.

### 1.2 Scope

The system records the attendance of people entering a controlled space, and controls a door latch based on whether the person is authorized.

A person identifies themselves in one of two ways: by standing in front of a camera, or by tapping an RFID card. The system determines who they are, records the event, and unlocks the door if they are authorized.

The system consists of one or more devices (nodes) and a server that performs face recognition, stores records, and manages the nodes. Each node operates in one of two modes assigned by the server: **Attendance mode** (installed at a door, identifies people and unlocks) or **Enrollment mode** (used by an administrator to capture faces and register cards for users). Administrators manage users, cards and nodes, and view reports, through a web interface.

**In scope:** attendance recording, door access control, user enrollment (face capture and card registration through a node), card management and card reuse, node management, attendance reporting, offline resilience.

**Not in scope for this release:** liveness or anti-spoofing detection, recognition of multiple people simultaneously, multi-site device management, an end-user mobile application, integration with external payroll or HR systems.

### 1.3 Definitions and Abbreviations

| Term | Meaning |
|---|---|
| **Node** (Edge AI Node) | A physical unit containing the camera, card reader, display, latch actuator, and processing hardware. |
| **Node mode** | The role a node currently performs, assigned by the server: Attendance or Enrollment. |
| **Attendance mode** | Normal operation at a door: identify by face or card, record attendance, unlock. |
| **Enrollment mode** | Operation as a registration station: capture face images and read card UIDs for a user selected by the administrator. No attendance is recorded and the latch never unlocks. |
| **Enrollment session** | One registration task (face capture or card registration) started by an administrator for a specific user on a specific Enrollment-mode node. |
| **Cloud Backend** | The server that performs face recognition, stores records, manages nodes, and hosts the administration interface. |
| **Local card store** | The copy of card-to-user authorization data held in the node's internal flash, synchronized from the server. |
| **Card release** | Removing a card from its current user so it returns to the pool of unassigned cards and can be assigned again. |
| **Face detection** | Determining whether a face is present in an image and where it is. Does not identify the person. |
| **Face recognition** | Determining the identity of a person from an image of their face. |
| **Face embedding** | A fixed-length numeric vector that represents a face. Two embeddings of the same person are numerically close; two embeddings of different people are far apart. |
| **Enrollment** | The process of registering a new person so the system can subsequently recognize them. |
| **UID** | The unique identifier stored on an RFID card. |
| **False acceptance** | The system grants access to a person who is not authorized. |
| **False rejection** | The system denies access to a person who is authorized. |
| **Cooldown** | A period after a recorded attendance event during which the same person's repeat presentations are not recorded again. |
| **Provisioning mode** | A temporary state in which the ESP32-C5 runs its own Wi-Fi access point and serves a configuration web page for entering network settings. |
| **Provisioning button** | The physical push button on the node that starts provisioning mode (CON-11). |
| **NPU** | Neural Processing Unit — hardware that accelerates AI model execution. |

### 1.4 References

- IEEE 830-1998, Recommended Practice for Software Requirements Specifications
- ISO/IEC/IEEE 29148:2018, Requirements engineering
- `ARCHITECTURE.md` — the companion design document for this system

### 1.5 How to Read This Document

Section 2 gives the general picture: what the product is, who uses it, and what limits it operates under. Section 3 states the requirements in detail. Each requirement has an identifier so it can be referenced in design documents, test plans, and change requests. Identifiers of superseded requirements are not reused.

Requirements use the word **shall** for mandatory behavior and **should** for recommended behavior.

---

## 2. Overall Description

### 2.1 Product Perspective

This is a new, self-contained system. It does not replace an existing system and does not depend on any external service beyond standard network infrastructure.

The system spans two physical locations. Nodes handle interaction with people. A server handles face identification, record keeping, and node management. The two communicate over a network connection.

The division of work between them is driven by a hardware constraint: the processor in the node can run a small model that finds faces in an image, but it cannot run the much larger model required to identify a person, nor can it hold face data for enrolled users. Face identification therefore happens on the server. Card authorization data is small enough to hold on the node, so card decisions are made locally from a server-synchronized copy, which keeps card access fast and available while offline.

### 2.2 Product Functions

At a high level the system provides:

**Attendance by face.** A person stands in front of the node. The node captures their face, shows it on screen while the server identifies them, records the attendance event, and unlocks the door if authorized.

**Attendance by card.** A person taps an RFID card. The node checks the card against its local card store, unlocks immediately if authorized, and reports the event to the server.

**Lock-style display.** When idle, the node shows the time and date like an ordinary electronic lock. It does not show a live camera image.

**Visual feedback.** The node tells the person what is happening at every moment: ready, verifying, granted, denied, or unavailable.

**Door control.** The node physically unlocks and relocks the door latch.

**Offline operation.** When the network is unavailable, the node continues to accept cards using its local card store, stores events locally, and uploads them when the connection returns.

**Node management.** Administrators register nodes, see their status, and switch each node between Attendance and Enrollment mode from the web interface.

**Enrollment through a node.** Administrators register a user's face and card using a node in Enrollment mode, guided step by step on both the web interface and the node display.

**User and card management.** Administrators register people, assign, release, and reassign cards, and deactivate people who leave.

**Reporting.** Administrators view and export attendance records.

### 2.3 User Classes

| User class | Description | Technical skill | Frequency of use |
|---|---|---|---|
| **Attendee** | An employee or student who uses the system to record attendance and enter the space. Interacts only with an Attendance-mode node, and with an Enrollment-mode node once when being registered. | None assumed. The interaction must be obvious without instruction. | Several times a day |
| **Administrator** | Registers users, captures faces, manages cards and nodes, runs reports. Uses the web interface and an Enrollment-mode node. | Comfortable with ordinary web applications. Not a technical specialist. | Daily to weekly |
| **System maintainer** | Installs nodes, configures thresholds, diagnoses faults. | Technical. | Rarely, after installation |

### 2.4 Operating Environment

Attendance-mode nodes operate indoors, at an entrance. Lighting conditions vary through the day and are not controlled. Enrollment-mode nodes are expected to be at an administrator's desk under indoor lighting. Ambient temperature is normal indoor range. Nodes are mains powered and connected to the site's wireless network.

The server operates in a normal server environment, either cloud-hosted or on-premise. The administration interface is accessed through a standard web browser on a desktop computer.

### 2.5 Design and Implementation Constraints

These are fixed inputs to the project, not choices open to the design team.

| ID | Constraint |
|---|---|
| CON-1 | The node shall be built on the NXP FRDM-MCXN947 development board. |
| CON-2 | Network connectivity shall be provided by an ESP32-C5 DevKit 2.0 module. |
| CON-3 | Image capture shall use an OV7670 camera module. |
| CON-4 | Card reading shall use an MFRC522 RFID reader operating at 13.56 MHz. |
| CON-5 | User feedback shall be presented on a 2.4 inch SPI TFT display (240×320). |
| CON-6 | The door latch shall be driven by a servo motor. |
| CON-7 | Face detection shall execute on the integrated NPU of the MCXN947. Detection shall not depend on the general-purpose CPU for its main computation. |
| CON-8 | Face recognition shall execute on the server. It shall not execute on the node. |
| CON-9 | Available flash memory on the node limits how much data can be buffered offline and how many cards the local card store can hold. Both shall be met within this limit. |
| CON-10 | Enrollment-mode and Attendance-mode nodes shall use the same hardware and the same firmware image. Mode is a runtime setting, not a separate build. |
| CON-11 | Provisioning mode shall be started by a physical push button on the FRDM-MCXN947. The requested button is **SW2**. On the current baseboard SW2 cannot be used: SW2 connects P0_23 directly to GND, and P0_23 is also ARD_A5, which carries `ESP32_CS` (SJ9 default 1-2, UM12018 Table 20 and Figure 6). Pressing SW2 would short the driven chip-select output to ground and corrupt SPI traffic to the ESP32-C5. Until the PCB moves `ESP32_CS` off A5 or adds a dedicated button, the provisioning button shall be **SW3** (P0_6, free in the current pinout). See OI-10. |

### 2.6 Assumptions and Dependencies

| ID | Assumption |
|---|---|
| ASM-1 | A wireless network with internet access is available at the installation site. |
| ASM-2 | The site accepts that biometric data will be processed, and has obtained any consent required by local law. |
| ASM-3 | The number of enrolled users at a single site will not exceed a few thousand. |
| ASM-4 | Attendees present themselves one at a time. The system is not required to handle a crowd. |
| ASM-5 | Attendees are cooperative — they are trying to be recognized, not trying to avoid it. |
| ASM-6 | Mains power is available and reasonably stable. |
| ASM-7 | During enrollment the administrator is physically present next to the Enrollment-mode node and can see both the node display and the web interface. |

---

## 3. Specific Requirements

### 3.1 External Interface Requirements

#### 3.1.1 User Interfaces — Node, Attendance mode

**UI-1.** *(Superseded in v2.1 — replaced by UI-7..UI-15.)*

**UI-2.** State shall be distinguishable by color and by icon as well as by text, so that the state is readable at a glance from a short distance and by people with color-vision deficiency.

**UI-3.** On a successful identification the display shall show the identified person's name.

**UI-4.** On a failed identification the display shall show a message indicating the person was not recognized. It shall not reveal any information about enrolled users.

**UI-5.** Result screens shall return to the idle screen automatically. No user action shall be required to reset the node.

**UI-7.** In the idle state the display shall show the current time and date, in the manner of an ordinary electronic door lock. The time shown shall be the node's synchronized time (CI-5).

**UI-8.** The display shall not show a live camera image in any Attendance-mode state. Face detection runs in the background without visual output.

**UI-9.** The idle screen shall show, unobtrusively, the node's connection status (online / offline) and a short hint that either face or card may be used.

**UI-10.** When an identification attempt is initiated by face (FR-2), the node shall capture a still image of the detected face and display it cropped inside a circular frame.

**UI-11.** While the server decision is pending, the circular frame shall be surrounded by an animated progress ring indicating that verification is in progress. The animation shall continue for as long as the node is waiting, so the person never sees a frozen screen.

**UI-12.** When the decision arrives, the progress ring shall change to the result: a success indication with the person's name (granted), or a failure indication with a "not recognized — please try again or use your card" message (denied). The captured face image shall remain in the circle during the result display, then be cleared.

**UI-13.** On a card tap, the node shall show the result immediately — without the waiting screen — using the local card decision (FR-10a): granted with the person's name, or denied.

**UI-14.** While offline, a face presentation shall produce a clear message that face verification is unavailable and that the card should be used instead (FR-24).

**UI-15.** The display shall at all times indicate one of: idle, verifying, granted, denied, offline, or service unavailable.

#### 3.1.2 User Interfaces — Node, Enrollment mode

**UI-16.** The display shall clearly and permanently indicate that the node is in Enrollment mode (for example, a distinct header color and label), so it is never mistaken for an Attendance-mode node.

**UI-17.** When no enrollment session is active, the display shall show that the node is waiting for the administrator to start a session from the web interface.

**UI-18.** During a face enrollment session, the display shall show the live camera image with a face-position guide (for example, an oval outline), the name of the user being enrolled, a step instruction (such as "look straight", "turn slightly left"), and progress (such as "2 / 5"). Live camera view is permitted in this mode only, to help the person position themselves.

**UI-19.** After each capture the display shall briefly confirm whether the image was accepted or rejected by the server, and if rejected, give the reason in plain language (for example "no face found — move closer").

**UI-20.** During a card registration session, the display shall show the user's name and prompt the person to tap the card, with a visible countdown until the session times out.

**UI-21.** At the end of a session the display shall show a clear summary (success, or what failed) and then return to the waiting screen.

#### 3.1.2a User Interfaces — Node, Provisioning mode

**UI-26.** While provisioning mode is active, the display shall show: a clear "Wi-Fi setup" title; the access point name and its password; the address of the configuration page; a QR code that lets a phone join the access point by scanning; and a countdown until provisioning mode times out.

**UI-27.** The display shall show the progress of each attempt to apply new settings: connecting to Wi-Fi, reaching the server, then success or the reason for failure in plain language (for example "Sai mật khẩu Wi-Fi", "Không tìm thấy server").

**UI-28.** The configuration web page shall be usable on a phone screen without zooming. It shall offer a list of nearby networks to pick from (with signal strength), a password field with a show/hide toggle, the server address pre-filled with the current value, and a single "Save and connect" button.

**UI-29.** The configuration web page shall show the result of the attempt (UI-27) on the page itself, so the user does not have to look back at the node.

#### 3.1.3 User Interfaces — Administration web

**UI-6.** The administration interface shall allow an administrator to complete user enrollment without technical assistance.

**UI-22.** The administration interface shall provide a node management page listing every node with its name, location, mode, online status, last-seen time, and local card store synchronization status.

**UI-23.** The administration interface shall provide a guided, step-by-step enrollment flow: select user (or create one) → select an Enrollment-mode node → capture faces → register card → confirm. Each step shall show what is expected of the administrator and the attendee.

**UI-24.** During a face enrollment session, the administration interface shall show each captured image with its accept/reject result, and allow the administrator to retake an individual image before confirming.

**UI-25.** Any destructive or security-relevant action (deleting a user, releasing or reassigning a card, switching a node's mode) shall require explicit confirmation, and the confirmation shall state the consequence in plain language.

#### 3.1.4 Hardware Interfaces

**HI-1.** The system shall capture still images from the camera module at a resolution sufficient for the face recognition process to operate within the accuracy targets in section 3.5.

**HI-2.** The system shall read the UID of an ISO/IEC 14443 Type A card presented within the reader's normal operating range.

**HI-3.** The system shall drive the door latch to an unlocked position and return it to a locked position.

**HI-4.** The latch shall be in the locked position whenever the system is not actively granting access, including after a power failure, a reset, or a software fault.

#### 3.1.5 Communication Interfaces

**CI-1.** All communication between nodes and the server shall be encrypted in transit.

**CI-2.** The server shall authenticate each node before accepting any data from it. Data from an unrecognized node shall be rejected.

**CI-3.** The node shall report its liveness to the server periodically so that outages are detectable.

**CI-4.** The node shall obtain its operational configuration and its mode from the server, so that behavior can be adjusted without reprogramming the node.

**CI-5.** The node shall maintain accurate time, synchronized from the network when available, so that the idle clock is correct and events recorded while offline carry correct timestamps.

**CI-6.** The server shall be able to deliver commands to a node (mode change, start/cancel enrollment session, card store update) without waiting for a manual action at the node. Delivery shall occur within the latency in PR-9.

**CI-7.** The address of the server and the Wi-Fi network the node joins shall be configurable on site through provisioning mode (§3.2.12), without reprogramming either processor.

---

### 3.2 Functional Requirements

#### 3.2.1 Face Attendance

**FR-1.** In Attendance mode, the node shall continuously monitor the camera for the presence of a face while idle, without displaying the camera image (UI-8).

**FR-2.** The node shall only initiate an identification attempt when a face has been present continuously for a short interval. It shall not initiate an attempt on a person merely passing by.

**FR-3.** When the identification attempt is initiated, the node shall capture the region of the image containing the face, display it as specified in UI-10/UI-11, and transmit it to the server.

**FR-4.** The server shall determine whether the received face corresponds to an enrolled, active user.

**FR-5.** The server shall return to the node a decision indicating whether access is granted, and if so, the identity of the person.

**FR-6.** Where more than one face is present, the system shall process the most prominent face and ignore the others.

**FR-7.** The node shall not attempt to determine identity from a face itself. All face identity decisions shall be made by the server.

#### 3.2.2 Card Attendance

**FR-8.** In Attendance mode, the node shall monitor for the presence of an RFID card while idle, concurrently with face monitoring. A person shall be able to use either method at any time without switching modes. A card tap during a pending face verification shall take priority.

**FR-9.** On detecting a card, the node shall read its UID.

**FR-10.** *(Superseded in v2.1 — replaced by FR-10a..FR-10c.)*

**FR-10a.** The node shall decide card access locally by looking up the UID in its local card store. Access shall be granted only if the UID is present and mapped to an active user.

**FR-10b.** The node shall report every card event (granted or denied, with UID and time of occurrence) to the server, so that it appears in attendance records and on the administration interface.

**FR-10c.** The server shall remain the single source of truth for card authorization. The local card store is a synchronized copy; it shall never be modified by the node on its own initiative.

**FR-11.** *(Superseded in v2.1 — authorization rule moved to FR-10a; server-side bookkeeping to FR-10b.)*

#### 3.2.3 Attendance Recording

**FR-12.** The system shall record every identification attempt in Attendance mode, whether successful or not, together with the time it occurred, the node where it occurred, the method used, and the outcome.

**FR-13.** A repeat presentation by the same person within a configurable cooldown period shall not create an additional attendance record.

**FR-14.** A repeat presentation within the cooldown period shall nevertheless grant access. Access shall not be denied on the grounds that attendance has already been recorded.

**FR-15.** Attendance records shall carry the time the event occurred at the node, which may differ from the time the server received it.

#### 3.2.4 Access Control

**FR-16.** On a granted decision, the node shall unlock the latch, hold it unlocked for a configurable period, and then relock it.

**FR-17.** On a denied decision, the latch shall not move.

**FR-18.** If the node does not receive a face decision from the server within a defined timeout, it shall treat the outcome as denied and shall not unlock.

#### 3.2.5 Offline Operation

**FR-19.** When the network is unavailable, the node shall remain operational and shall inform the user of the degraded state (UI-9, UI-14).

**FR-20.** The node shall store attendance events that could not be transmitted, and shall transmit them once connectivity is restored.

**FR-21.** Stored events shall retain the time at which they originally occurred.

**FR-22.** Events shall be removed from local storage only after the server has confirmed receipt.

**FR-23.** *(Superseded in v2.1 — card access always uses the local card store (FR-10a), online or offline. Offline revocation latency is covered by FR-53 and OI-7.)*

**FR-24.** Face-based access shall not be granted while offline, since identification cannot be performed on the node.

**FR-25.** If local storage for offline events becomes full, the node shall retain the oldest events and inform the user that new events cannot be recorded.

#### 3.2.6 Enrollment and User Management

**FR-26.** An administrator shall be able to register a new user with a name and an organizational identifier.

**FR-27.** Enrollment shall accept several images of the user's face, so that the stored representation is not dependent on a single lighting condition or pose.

**FR-28.** The system shall reject an enrollment image in which no face can be found, and shall tell the administrator why.

**FR-29.** An administrator shall be able to assign an RFID card to a user, release it, and reassign it (see §3.2.11).

**FR-30.** An administrator shall be able to deactivate a user. A deactivated user shall be denied access by both methods, immediately on the server and on each online node within PR-9, without any action at the node.

**FR-31.** An administrator shall be able to permanently delete a user together with all of their personal data. Any card assigned to the user shall be released (FR-56).

#### 3.2.7 Reporting and Monitoring

**FR-32.** An administrator shall be able to view attendance records filtered by user, by node, and by date range.

**FR-33.** An administrator shall be able to export attendance records in a format suitable for use in a spreadsheet.

**FR-34.** An administrator shall be able to see, for each node, when it last communicated with the server.

**FR-35.** The system shall record failed identification attempts and make them visible to administrators, so that repeated unauthorized attempts can be noticed.

**FR-36.** Card events uploaded by nodes (FR-10b), including those buffered offline, shall appear in attendance records identically to face events, with the method recorded as card.

#### 3.2.8 Node Management

**FR-37.** An administrator shall be able to register a new node, giving it a name and location, and obtain the credential the node uses to authenticate (CI-2).

**FR-38.** An administrator shall be able to rename, relocate, disable, or remove a node. A disabled or removed node shall be refused by the server.

**FR-39.** An administrator shall be able to set the mode of each node to Attendance or Enrollment from the administration interface. The node shall apply the change without reprogramming or a manual action at the node.

**FR-40.** A newly registered node shall start in Attendance mode unless the administrator selects otherwise.

**FR-41.** A node shall persist its last assigned mode across restarts, and shall confirm its current mode to the server after each restart and after each change.

**FR-42.** A mode change shall not be applied while an identification or enrollment session is in progress on that node; it shall take effect when the node next returns to idle.

**FR-43.** The administration interface shall show, for each node: mode, online/offline status, last-seen time, firmware version, local card store version compared with the server's current version, and pending offline events count.

#### 3.2.9 Enrollment Mode Operation

**FR-44.** In Enrollment mode the node shall not perform attendance identification, shall not record attendance events, and shall never unlock the latch.

**FR-45.** Every enrollment action shall be initiated by an authenticated administrator from the administration interface, by selecting a user and an Enrollment-mode node. A node shall never start an enrollment session on its own, and no enrollment shall be possible from a node in Attendance mode.

**FR-46.** Only one enrollment session shall be active on a node at a time. A session shall end on completion, on cancellation from the administration interface, or after a configurable inactivity timeout; in every case the node shall return to its waiting screen.

**FR-47.** **Face enrollment session.** The node shall capture a configurable number of face images (default 5) following the guided poses (UI-18). Each image shall be sent to the server, which shall accept or reject it (FR-28) and report the result to both the node and the administration interface. The user's face representation shall be created or replaced only after the administrator confirms the session; cancelling shall discard all captured images. Re-enrolling an existing user shall replace their previous face representation only on confirmation.

**FR-48.** **Card registration session.** The node shall wait for a card tap and send the UID to the server. The server shall:
- assign the card if it is unassigned;
- if the card is already assigned to the same user, report "already registered" and make no change;
- if the card is assigned to another user, show the administrator who currently holds it and require explicit confirmation before reassigning it (FR-55).

The result shall be shown on both the node and the administration interface.

**FR-49.** Enrollment images shall be transmitted encrypted, shall not be stored on the node beyond the session, and shall be discarded by the server after the face representation is derived (DR-2, SR-3).

**FR-50.** The system shall also allow face images to be uploaded directly through the administration interface (without a node), as a fallback when no Enrollment-mode node is available. The same acceptance rules (FR-28) apply.

#### 3.2.10 Local Card Store Synchronization

**FR-51.** Each Attendance-mode node shall hold a local card store in internal flash containing, for every assigned card: UID, user identifier, display name, and active flag. It shall contain no face data.

**FR-52.** The server shall maintain a version number for card authorization data, incremented on every change (assign, release, reassign, user activation/deactivation, user rename, user deletion).

**FR-53.** When card authorization data changes, the server shall notify online nodes, and each node shall update its local card store to the new version within PR-9. Nodes that were offline shall update upon reconnecting, before or immediately after uploading their buffered events.

**FR-54.** A local card store update shall be atomic: a power loss or communication failure during the update shall leave the node with either the complete previous version or the complete new version, never a mix.

**FR-54a.** The node shall report its local card store version in each heartbeat, so that the server and administrators can see which nodes are out of date (FR-43).

#### 3.2.11 Card Reuse

**FR-55.** A card shall be reusable. An administrator shall be able to:
- **release** a card from its user, returning it to the unassigned pool;
- **reassign** a card directly from one user to another (with confirmation, UI-25);
- register a previously released card to any user through a card registration session (FR-48) or by entering its UID.

**FR-56.** When a user is deleted or their card is released or reassigned, the card shall immediately stop granting access for the previous user — on the server at once, and on nodes within PR-9 (FR-53).

**FR-57.** Releasing or reassigning a card shall not alter past attendance records. Records made with the card before the change shall remain attributed to the user who held it at the time of the event, judged by the event's occurrence time.

**FR-58.** An administrator should be able to mark a card as **lost**. A lost card shall be denied everywhere, shall not be assignable until the mark is removed, and its subsequent presentations shall be flagged in the failed-attempt view (FR-35).

**FR-59.** The system shall keep an assignment history for each card (which user, from when, until when, and which administrator made each change).

#### 3.2.12 Wi-Fi Provisioning

**FR-60.** Pressing and holding the provisioning button (CON-11) for a configurable time (default 3 seconds) shall put the node into provisioning mode. A short press shall have no effect, so that an accidental touch does not change anything. The hold shall be ignored while the node is booting (pressing SW3 during reset selects ISP boot mode).

**FR-61.** On entering provisioning mode, the MCXN947 shall instruct the ESP32-C5 to start a Wi-Fi access point and serve the configuration web page. The configuration page shall open automatically when a phone joins the access point (captive portal), and shall also be reachable at a fixed address shown on the display.

**FR-62.** The configuration page shall let the user enter: the Wi-Fi network name (picked from a scan list or typed in for hidden networks), the Wi-Fi password, and the server address. The server address field shall be pre-filled with the current value.

**FR-63.** On "Save and connect", the node shall try the new settings before keeping them: join the Wi-Fi network, then reach the server's health endpoint. New settings shall be stored only if both steps succeed. On failure, the previous settings shall be kept, the failure reason shown (UI-27, UI-29), and the user allowed to try again without leaving provisioning mode.

**FR-64.** On success the node shall leave provisioning mode, reconnect using the new settings, and resume normal operation in its assigned mode.

**FR-65.** Provisioning mode shall end without changes if: the timeout expires (configurable, default 5 minutes), the provisioning button is held again, or no device has joined the access point within the timeout. The node shall then reconnect with its previous settings.

**FR-66.** Wi-Fi and server settings shall persist across power loss and restart.

**FR-67.** While in provisioning mode the node is offline (FR-19). Card access from the local card store shall continue and card events shall be buffered (FR-20). Face verification shall be unavailable (FR-24). The latch shall remain locked except for a granted card.

**FR-68.** Provisioning mode shall be available in both Attendance and Enrollment mode. An enrollment session in progress shall be cancelled cleanly (RR-6) when provisioning starts.

**FR-69.** After a successful change, the node shall report to the server that its network settings were changed through provisioning, with the time, so that administrators can see it in the node's history.

**FR-70.** A node with no stored Wi-Fi settings (first boot) shall show a screen asking for setup with the provisioning button, instead of repeatedly failing to connect.

---

### 3.3 Data Requirements

This section states what information the system must hold, not how it is stored.

**DR-1.** For each enrolled person the system shall hold: their name, an organizational identifier, their face representation, an optional card identifier, and whether their account is active.

**DR-2.** The face representation shall be stored in a form derived from the enrollment images. **Original enrollment images shall not be retained after the representation has been derived.**

**DR-3.** The stored face representation shall be associated with the version of the recognition process that produced it, so that representations can be regenerated if that process changes.

**DR-4.** For each attendance event the system shall hold: which person (where identified), which node, which method, the card UID where applicable, the outcome, the confidence of the match where applicable, the time the event occurred, and the time it was received.

**DR-5.** For each node the system shall hold: an identifier, name, physical location, authentication credential, mode, enabled flag, firmware version, local card store version, and the time it was last seen.

**DR-6.** A card identifier shall be assigned to at most one person at any time.

**DR-7.** Attendance records shall be retained for a configurable period, after which they may be archived or purged in accordance with site policy.

**DR-8.** For each card known to the system the server shall hold: UID, state (unassigned, assigned, lost), current user if assigned, and assignment history (FR-59).

**DR-9.** The node's local card store shall hold only the data listed in FR-51 plus the store version number. Display names are the only personal data held on the node.

---

### 3.4 Performance Requirements

**PR-1.** The interval from a person presenting their face to the door beginning to unlock shall not exceed **2 seconds** under normal network conditions.

**PR-2.** The interval from a card being tapped to the door beginning to unlock shall not exceed **0.5 seconds**, online or offline (local decision, FR-10a).

**PR-3.** The node shall evaluate camera frames for the presence of a face at a rate of at least **5 frames per second**.

**PR-4.** The server shall complete the recognition process for a single face within **500 milliseconds**.

**PR-5.** The server shall support at least **10 nodes** submitting events concurrently without exceeding PR-4.

**PR-6.** The node shall return to the idle screen within **1 second** of completing a result display.

**PR-7.** The captured face (UI-10) and the progress ring (UI-11) shall appear within **300 milliseconds** of the identification attempt being initiated.

**PR-8.** The local card store shall hold at least **ASM-3's user count** (target: 2,000 cards), subject to OI-6.

**PR-9.** A change made on the server (mode change, card change, user deactivation, enrollment session start) shall take effect on an online node within **10 seconds**.

**PR-10.** An Enrollment-mode node shall show the accept/reject result of each captured face image within **2 seconds** of capture.

**PR-11.** The access point and the configuration page shall be available within **10 seconds** of the provisioning button being held. A successful "Save and connect" shall complete within **30 seconds**.

---

### 3.5 Accuracy Requirements

**AR-1.** The false acceptance rate shall not exceed **0.1%**. A false acceptance is a security failure and is the more serious of the two error types.

**AR-2.** The false rejection rate should not exceed **5%**. A false rejection is an inconvenience; the person may retry or use their card.

**AR-3.** The decision threshold that governs the trade-off between AR-1 and AR-2 shall be configurable without reprogramming the node or redeploying the server.

**AR-4.** The accuracy targets apply under the lighting conditions present at the installation site. Acceptance testing shall be performed under those conditions, not under laboratory lighting.

**AR-5.** The system shall record the confidence of each successful match, so that threshold tuning can be based on real operating data.

---

### 3.6 Safety and Security Requirements

**SR-1.** Face images and face representations shall be treated as sensitive personal data throughout the system.

**SR-2.** Face images shall never be transmitted or stored unencrypted.

**SR-3.** Face images captured on the node — for attendance (UI-10) or enrollment (FR-47) — shall be erased from the node as soon as they have been transmitted and the corresponding screen has been cleared. They shall never be written to the node's flash or SD card.

**SR-4.** The administration interface shall require authentication. Only authenticated administrators shall be able to view or modify user data, card data, node settings, or attendance records, or start enrollment sessions.

**SR-5.** A person's complete personal data shall be removable from the system on request, in support of data protection obligations. Removal shall also remove the person's entry from every node's local card store (FR-53).

**SR-6.** The door latch shall fail to the locked state. No fault condition — power loss, software crash, network failure, peripheral failure, or a corrupt local card store — shall leave the door unlocked.

**SR-7.** The system does not detect presentation attacks. **A photograph of an enrolled person may be sufficient to obtain access.** This is a known limitation of this release and shall be documented for anyone deploying the system. Sites requiring protection against this shall not rely on face recognition as the sole access control.

**SR-8.** Denial messages shall not disclose whether a presented face or card is close to any enrolled record.

**SR-9.** The local card store shall be protected against tampering: the node shall verify its integrity before use, and shall treat a store that fails verification as empty (deny all cards) and request a fresh copy from the server.

**SR-10.** A node in Enrollment mode shall not be usable to grant access to anything. Enrollment commands shall be accepted by a node only from the authenticated server.

**SR-11.** RFID card UIDs are not secret and can be cloned. Card-based access therefore provides only the security of a basic proximity card; this shall be documented for anyone deploying the system.


**SR-12.** The provisioning access point shall be protected by WPA2 (or better) with a random password generated for each provisioning session and shown only on the node display (UI-26). Only someone standing at the node can join it.

**SR-13.** The configuration page shall never display or return the stored Wi-Fi password or the node's server credential. Stored Wi-Fi and server settings should be kept in encrypted storage on the ESP32-C5.

**SR-14.** The provisioning button shall not be reachable from outside the node enclosure in a deployed installation, so that a passer-by cannot take the node offline or redirect it to another server.
---

### 3.7 Reliability and Availability Requirements

**RR-1.** The node shall recover automatically from a loss of network connectivity, a server timeout, or a peripheral communication failure, without manual intervention.

**RR-2.** The node shall detect and recover from a software hang without manual intervention.

**RR-3.** After any restart, the node shall return to a known safe state with the latch locked, in its last assigned mode.

**RR-4.** No attendance event that has been accepted by the node shall be lost as a result of a network outage or a node restart.

**RR-5.** The node shall be able to operate offline for at least **24 hours** without loss of recorded card events.

**RR-6.** An enrollment session interrupted by a network loss or node restart shall be cancelled cleanly on both sides, without creating a partial face representation or a partial card assignment.

---

### 3.8 Maintainability and Configurability Requirements

**MR-1.** The following shall be configurable from the server without reprogramming the node: the recognition decision threshold, the door unlock duration, the attendance cooldown period, the server request timeout, the number of face images per enrollment, the enrollment session timeout, and the result-screen display duration.

**MR-2.** The recognition model on the server shall be replaceable without changing the node firmware.

**MR-3.** The system shall log sufficient diagnostic information for a maintainer to determine the cause of a failed identification, a failed enrollment, or a failed card store synchronization without physical access to the node.

---

### 3.9 Usability Requirements

**UR-1.** An attendee shall be able to use the system correctly on their first attempt without instruction.

**UR-2.** The node shall indicate a result within the time limits of PR-1 and PR-2, or shall indicate that it is working (UI-11), so that the user is never left uncertain whether the node has responded.

**UR-3.** An administrator shall be able to enroll a new user — create the user, capture faces, and register a card — in under three minutes using an Enrollment-mode node.

**UR-4.** Text on the node display shall be legible from at least **1 meter**. Names and result messages shall use large type; secondary information (status, hints) may be smaller but shall remain readable.

**UR-5.** All node and administration interface text shall be in the site's language, Vietnamese by default, with English as an option. Messages shall be short, polite, and tell the person what to do next (for example "Không nhận diện được — vui lòng thử lại hoặc quẹt thẻ").

**UR-6.** Each result shall use a consistent combination of color, icon, and text across all screens: green with a check mark for granted, red with a cross for denied, amber for offline or warning, blue for verifying or in-progress.

**UR-7.** Transitions between screens shall be smooth and no screen shall appear frozen; any wait longer than 300 ms shall show a progress indication.

**UR-8.** Error messages on the administration interface shall state what went wrong and how to fix it, in non-technical language. No raw error codes or stack traces shall be shown to administrators.

**UR-9.** The administration interface shall be usable on a desktop browser and should remain usable on a tablet-sized screen.

**UR-10.** Switching a node's mode, releasing a card, or reassigning a card shall each be achievable in no more than three clicks from the relevant list page, plus the confirmation (UI-25).

---

## 4. Verification

Each requirement shall be verifiable. The following table shows how each category will be confirmed.

| Requirement group | Verification method |
|---|---|
| Functional (FR) | Functional testing against each requirement, on assembled hardware |
| Performance (PR) | Timed measurement on the target hardware under representative load |
| Accuracy (AR) | Statistical testing with a test population under site lighting conditions |
| Security (SR) | Inspection, network capture analysis, and penetration testing of the administration interface |
| Reliability (RR) | Fault injection — network disconnection, power cycling during card store update and enrollment, peripheral disconnection |
| Usability (UR, UI) | Observation of untrained users completing the task; screen review against UR-4..UR-7 |

Accuracy requirements in particular shall not be considered met on the basis of published model benchmarks. They shall be measured on this system, with this camera, at the installation site.

---

## 5. Open Issues

These must be resolved before the design is finalized. Each has been raised because it materially affects whether the requirements above can be met.

| ID | Issue | Why it matters |
|---|---|---|
| OI-1 | The camera specified in CON-3 has limited low-light performance. | May prevent AR-1 and AR-2 being met at an entrance in evening conditions. Requires early testing; may require supplementary illumination or a different camera. |
| OI-2 | Available bandwidth between the node and the network module has not been measured. | Directly determines whether PR-1, PR-7 and PR-10 can be met when transmitting an image. |
| OI-3 | Available flash for offline buffering has not been quantified against CON-9. | Determines whether RR-5 is achievable, and whether face events can be buffered at all. |
| OI-4 | The retention period for attendance records (DR-7) has not been set. | Depends on site policy and applicable data protection law. |
| OI-5 | Whether the site requires protection against presentation attacks (SR-7). | If it does, the current scope is insufficient and liveness detection must be added to scope. |
| OI-6 | Internal flash capacity for the local card store has not been quantified alongside the offline event buffer (CON-9). | Determines whether PR-8 is achievable; may require using the SD card for the offline event buffer to free internal flash. |
| OI-7 | A node that is offline cannot learn of card revocations until it reconnects. | A released, reassigned, or lost card, or a deactivated user's card, may still open an offline node during that window. The site must accept this, or a maximum offline age for the card store must be defined after which card access is refused. |
| OI-8 | The live camera preview required in Enrollment mode (UI-18) shares the SPI bus with the card reader and network module. | Preview frame rate may be low; must be measured. Acceptable as long as UR-3 is met. |
| OI-9 | The display module includes a resistive touch controller (XPT2046). Touch input is not required by this release. | Could later allow on-node actions (e.g. "retake", "cancel") in Enrollment mode; out of scope until confirmed useful. |
| OI-10 | Provisioning button hardware (CON-11). SW2 shares P0_23 with `ESP32_CS` on the current baseboard; SW3 is the interim choice. | Decide in the next PCB revision whether to move `ESP32_CS` off A5 (frees SW2) or add a dedicated, enclosure-internal button. Also note: the 0.1 µF capacitor C4 on P0_23 currently loads the `ESP32_CS` line and slows its edges, a further reason to move CS off this pin. |
| OI-11 | How a brand-new node receives its server credential (CI-2). | Provisioning mode sets network and server address only. If credential entry is also wanted on the configuration page, SR-13 must be extended to protect it. |
