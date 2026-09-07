# Software Requirements Specification
## Face and RFID Attendance System

**Version:** 2.0
**Status:** Draft for review
**Structure:** Based on IEEE 830 / ISO-IEC-IEEE 29148, adapted for an embedded system

---

## 1. Introduction

### 1.1 Purpose

This document states what the Face and RFID Attendance System must do and how well it must do it. It is written for the engineers who will design and build the system, and for the stakeholders who need to confirm that the right product is being built.

This document describes **requirements only**. It does not describe how those requirements will be met. Design decisions — module structure, interfaces, algorithms, data formats — belong in the architecture and design documents.

### 1.2 Scope

The system records the attendance of people entering a controlled space, and controls a door latch based on whether the person is authorized.

A person identifies themselves in one of two ways: by standing in front of a camera, or by tapping an RFID card. The system determines who they are, records the event, and unlocks the door if they are authorized.

The system consists of a device installed at the door and a server that performs recognition and stores records. Administrators manage users and view reports through a web interface.

**In scope:** attendance recording, door access control, user enrollment, card management, attendance reporting, offline resilience.

**Not in scope for this release:** liveness or anti-spoofing detection, recognition of multiple people simultaneously, multi-site device management, an end-user mobile application, integration with external payroll or HR systems.

### 1.3 Definitions and Abbreviations

| Term | Meaning |
|---|---|
| **Edge AI Node** | The physical unit installed at the door. Contains the camera, card reader, display, latch actuator, and processing hardware. |
| **Cloud Backend** | The server that performs face recognition, stores records, and hosts the administration interface. |
| **Face detection** | Determining whether a face is present in an image and where it is. Does not identify the person. |
| **Face recognition** | Determining the identity of a person from an image of their face. |
| **Face embedding** | A fixed-length numeric vector that represents a face. Two embeddings of the same person are numerically close; two embeddings of different people are far apart. |
| **Enrollment** | The process of registering a new person so the system can subsequently recognize them. |
| **UID** | The unique identifier stored on an RFID card. |
| **False acceptance** | The system grants access to a person who is not authorized. |
| **False rejection** | The system denies access to a person who is authorized. |
| **Cooldown** | A period after a recorded attendance event during which the same person's repeat presentations are not recorded again. |
| **NPU** | Neural Processing Unit — hardware that accelerates AI model execution. |

### 1.4 References

- IEEE 830-1998, Recommended Practice for Software Requirements Specifications
- ISO/IEC/IEEE 29148:2018, Requirements engineering
- `ARCHITECTURE.md` — the companion design document for this system

### 1.5 How to Read This Document

Section 2 gives the general picture: what the product is, who uses it, and what limits it operates under. Section 3 states the requirements in detail. Each requirement has an identifier so it can be referenced in design documents, test plans, and change requests.

Requirements use the word **shall** for mandatory behavior and **should** for recommended behavior.

---

## 2. Overall Description

### 2.1 Product Perspective

This is a new, self-contained system. It does not replace an existing system and does not depend on any external service beyond standard network infrastructure.

The system spans two physical locations. A device at the door handles interaction with the person. A server handles identification and record keeping. The two communicate over a network connection.

The division of work between them is driven by a hardware constraint: the processor in the door device can run a small model that finds faces in an image, but it cannot run the much larger model required to identify a person, nor can it hold the database of enrolled users. Identification therefore happens on the server.

### 2.2 Product Functions

At a high level the system provides:

**Attendance by face.** A person stands in front of the device. The system determines their identity from their face, records the attendance event, and unlocks the door if authorized.

**Attendance by card.** A person taps an RFID card. The system determines their identity from the card, records the event, and unlocks the door if authorized.

**Visual feedback.** The device tells the person what is happening at every moment: ready, working, granted, denied, or unavailable.

**Door control.** The device physically unlocks and relocks the door latch.

**Offline operation.** When the network is unavailable, the device continues to accept card presentations, stores the events locally, and uploads them when the connection returns.

**User management.** Administrators register new people, capture their faces, assign and revoke cards, and deactivate people who leave.

**Reporting.** Administrators view and export attendance records.

**Device monitoring.** Administrators can see whether each door device is online.

### 2.3 User Classes

| User class | Description | Technical skill | Frequency of use |
|---|---|---|---|
| **Attendee** | An employee or student who uses the system to record attendance and enter the space. Interacts only with the door device. | None assumed. The interaction must be obvious without instruction. | Several times a day |
| **Administrator** | Registers users, manages cards, runs reports. Uses the web interface. | Comfortable with ordinary web applications. Not a technical specialist. | Daily to weekly |
| **System maintainer** | Installs devices, configures thresholds, diagnoses faults. | Technical. | Rarely, after installation |

### 2.4 Operating Environment

The door device operates indoors, at an entrance. Lighting conditions vary through the day and are not controlled. Ambient temperature is normal indoor range. The device is mains powered and connected to the site's wireless network.

The server operates in a normal server environment, either cloud-hosted or on-premise. The administration interface is accessed through a standard web browser on a desktop computer.

### 2.5 Design and Implementation Constraints

These are fixed inputs to the project, not choices open to the design team.

| ID | Constraint |
|---|---|
| CON-1 | The door device shall be built on the NXP FRDM-MCXN947 development board. |
| CON-2 | Network connectivity shall be provided by an ESP32-C5 DevKit 2.0 module. |
| CON-3 | Image capture shall use an OV7670 camera module. |
| CON-4 | Card reading shall use an MFRC522 RFID reader operating at 13.56 MHz. |
| CON-5 | User feedback shall be presented on a 2.4 inch SPI TFT display. |
| CON-6 | The door latch shall be driven by a servo motor. |
| CON-7 | Face detection shall execute on the integrated NPU of the MCXN947. Detection shall not depend on the general-purpose CPU for its main computation. |
| CON-8 | Face recognition shall execute on the server. It shall not execute on the door device. |
| CON-9 | Available flash memory on the device limits how much data can be buffered offline. The offline buffering requirements shall be met within this limit. |

### 2.6 Assumptions and Dependencies

| ID | Assumption |
|---|---|
| ASM-1 | A wireless network with internet access is available at the installation site. |
| ASM-2 | The site accepts that biometric data will be processed, and has obtained any consent required by local law. |
| ASM-3 | The number of enrolled users at a single site will not exceed a few thousand. |
| ASM-4 | Attendees present themselves one at a time. The system is not required to handle a crowd. |
| ASM-5 | Attendees are cooperative — they are trying to be recognized, not trying to avoid it. |
| ASM-6 | Mains power is available and reasonably stable. |

---

## 3. Specific Requirements

### 3.1 External Interface Requirements

#### 3.1.1 User Interfaces

**UI-1.** The device display shall at all times show which of the following states the system is in: ready, processing, access granted, access denied, or service unavailable.

**UI-2.** State shall be distinguishable by color as well as by text, so that the state is readable at a glance from a short distance.

**UI-3.** On a successful identification the display shall show the identified person's name.

**UI-4.** On a failed identification the display shall show a message indicating the person was not recognized. It shall not reveal any information about enrolled users.

**UI-5.** Result screens shall return to the ready state automatically. No user action shall be required to reset the device.

**UI-6.** The administration interface shall allow an administrator to complete user enrollment without technical assistance.

#### 3.1.2 Hardware Interfaces

**HI-1.** The system shall capture still images from the camera module at a resolution sufficient for the face recognition process to operate within the accuracy targets in section 3.4.

**HI-2.** The system shall read the UID of an ISO/IEC 14443 Type A card presented within the reader's normal operating range.

**HI-3.** The system shall drive the door latch to an unlocked position and return it to a locked position.

**HI-4.** The latch shall be in the locked position whenever the system is not actively granting access, including after a power failure, a reset, or a software fault.

#### 3.1.3 Communication Interfaces

**CI-1.** All communication between the door device and the server shall be encrypted in transit.

**CI-2.** The server shall authenticate each door device before accepting any data from it. Data from an unrecognized device shall be rejected.

**CI-3.** The device shall report its liveness to the server periodically so that outages are detectable.

**CI-4.** The device shall obtain its operational configuration from the server, so that behavior can be adjusted without reprogramming the device.

**CI-5.** The device shall maintain accurate time, synchronized from the network when available, so that events recorded while offline carry correct timestamps.

---

### 3.2 Functional Requirements

#### 3.2.1 Face Attendance

**FR-1.** The device shall continuously monitor the camera for the presence of a face while in the ready state.

**FR-2.** The device shall only initiate an identification attempt when a face has been present continuously for a short interval. It shall not initiate an attempt on a person merely passing by.

**FR-3.** When the identification attempt is initiated, the device shall capture the region of the image containing the face and transmit it to the server.

**FR-4.** The server shall determine whether the received face corresponds to an enrolled, active user.

**FR-5.** The server shall return to the device a decision indicating whether access is granted, and if so, the identity of the person.

**FR-6.** Where more than one face is present, the system shall process the most prominent face and ignore the others.

**FR-7.** The device shall not attempt to determine identity itself. All identity decisions shall be made by the server.

#### 3.2.2 Card Attendance

**FR-8.** The device shall monitor for the presence of an RFID card while in the ready state, concurrently with face monitoring. A person shall be able to use either method at any time without switching modes.

**FR-9.** On detecting a card, the device shall read its UID and transmit it to the server.

**FR-10.** The device shall not evaluate whether a card UID is authorized. That decision shall be made by the server, so that a card can be revoked centrally and take effect immediately.

**FR-11.** The server shall grant access only if the UID belongs to an enrolled user whose account is active.

#### 3.2.3 Attendance Recording

**FR-12.** The system shall record every identification attempt, whether successful or not, together with the time it occurred, the device where it occurred, the method used, and the outcome.

**FR-13.** A repeat presentation by the same person within a configurable cooldown period shall not create an additional attendance record.

**FR-14.** A repeat presentation within the cooldown period shall nevertheless grant access. Access shall not be denied on the grounds that attendance has already been recorded.

**FR-15.** Attendance records shall carry the time the event occurred at the device, which may differ from the time the server received it.

#### 3.2.4 Access Control

**FR-16.** On a granted decision, the device shall unlock the latch, hold it unlocked for a configurable period, and then relock it.

**FR-17.** On a denied decision, the latch shall not move.

**FR-18.** If the device does not receive a decision from the server within a defined timeout, it shall treat the outcome as denied and shall not unlock.

#### 3.2.5 Offline Operation

**FR-19.** When the network is unavailable, the device shall remain operational and shall inform the user of the degraded state.

**FR-20.** The device shall store attendance events that could not be transmitted, and shall transmit them once connectivity is restored.

**FR-21.** Stored events shall retain the time at which they originally occurred.

**FR-22.** Events shall be removed from local storage only after the server has confirmed receipt.

**FR-23.** The system shall provide a configurable option to grant card-based access while offline, using authorization data cached from the last successful synchronization. This option shall be disabled by default.

**FR-24.** Face-based access shall not be granted while offline, since identification cannot be performed on the device.

**FR-25.** If local storage for offline events becomes full, the device shall retain the oldest events and inform the user that new events cannot be recorded.

#### 3.2.6 Enrollment and User Management

**FR-26.** An administrator shall be able to register a new user with a name and an organizational identifier.

**FR-27.** Enrollment shall accept several images of the user's face, so that the stored representation is not dependent on a single lighting condition or pose.

**FR-28.** The system shall reject an enrollment image in which no face can be found, and shall tell the administrator why.

**FR-29.** An administrator shall be able to assign an RFID card to a user, change it, and revoke it.

**FR-30.** An administrator shall be able to deactivate a user. A deactivated user shall be denied access by both methods, immediately, without any action at the device.

**FR-31.** An administrator shall be able to permanently delete a user together with all of their personal data.

#### 3.2.7 Reporting and Monitoring

**FR-32.** An administrator shall be able to view attendance records filtered by user and by date range.

**FR-33.** An administrator shall be able to export attendance records in a format suitable for use in a spreadsheet.

**FR-34.** An administrator shall be able to see, for each installed device, when it last communicated with the server.

**FR-35.** The system shall record failed identification attempts and make them visible to administrators, so that repeated unauthorized attempts can be noticed.

---

### 3.3 Data Requirements

This section states what information the system must hold, not how it is stored.

**DR-1.** For each enrolled person the system shall hold: their name, an organizational identifier, their face representation, an optional card identifier, and whether their account is active.

**DR-2.** The face representation shall be stored in a form derived from the enrollment images. **Original enrollment images shall not be retained after the representation has been derived.**

**DR-3.** The stored face representation shall be associated with the version of the recognition process that produced it, so that representations can be regenerated if that process changes.

**DR-4.** For each attendance event the system shall hold: which person (where identified), which device, which method, the outcome, the confidence of the match where applicable, the time the event occurred, and the time it was received.

**DR-5.** For each installed device the system shall hold: an identifier, its physical location, its authentication credential, and the time it was last seen.

**DR-6.** A card identifier shall be assigned to at most one person at any time.

**DR-7.** Attendance records shall be retained for a configurable period, after which they may be archived or purged in accordance with site policy.

---

### 3.4 Performance Requirements

**PR-1.** The interval from a person presenting their face to the door beginning to unlock shall not exceed **2 seconds** under normal network conditions.

**PR-2.** The interval from a card being tapped to the door beginning to unlock shall not exceed **1 second**.

**PR-3.** The device shall evaluate camera frames for the presence of a face at a rate of at least **5 frames per second**.

**PR-4.** The server shall complete the recognition process for a single face within **500 milliseconds**.

**PR-5.** The server shall support at least **10 door devices** submitting events concurrently without exceeding PR-4.

**PR-6.** The device shall resume the ready state within **1 second** of completing a result display.

---

### 3.5 Accuracy Requirements

**AR-1.** The false acceptance rate shall not exceed **0.1%**. A false acceptance is a security failure and is the more serious of the two error types.

**AR-2.** The false rejection rate should not exceed **5%**. A false rejection is an inconvenience; the person may retry or use their card.

**AR-3.** The decision threshold that governs the trade-off between AR-1 and AR-2 shall be configurable without reprogramming the device or redeploying the server.

**AR-4.** The accuracy targets apply under the lighting conditions present at the installation site. Acceptance testing shall be performed under those conditions, not under laboratory lighting.

**AR-5.** The system shall record the confidence of each successful match, so that threshold tuning can be based on real operating data.

---

### 3.6 Safety and Security Requirements

**SR-1.** Face images and face representations shall be treated as sensitive personal data throughout the system.

**SR-2.** Face images shall never be transmitted or stored unencrypted.

**SR-3.** Face images buffered on the device shall be erased as soon as they have been successfully transmitted.

**SR-4.** The administration interface shall require authentication. Only authenticated administrators shall be able to view or modify user data or attendance records.

**SR-5.** A person's complete personal data shall be removable from the system on request, in support of data protection obligations.

**SR-6.** The door latch shall fail to the locked state. No fault condition — power loss, software crash, network failure, or peripheral failure — shall leave the door unlocked.

**SR-7.** The system does not detect presentation attacks. **A photograph of an enrolled person may be sufficient to obtain access.** This is a known limitation of this release and shall be documented for anyone deploying the system. Sites requiring protection against this shall not rely on face recognition as the sole access control.

**SR-8.** Denial messages shall not disclose whether a presented face or card is close to any enrolled record.

---

### 3.7 Reliability and Availability Requirements

**RR-1.** The device shall recover automatically from a loss of network connectivity, a server timeout, or a peripheral communication failure, without manual intervention.

**RR-2.** The device shall detect and recover from a software hang without manual intervention.

**RR-3.** After any restart, the device shall return to a known safe state with the latch locked.

**RR-4.** No attendance event that has been accepted by the device shall be lost as a result of a network outage or a device restart.

**RR-5.** The device shall be able to operate offline for at least **24 hours** without loss of recorded card events.

---

### 3.8 Maintainability and Configurability Requirements

**MR-1.** The following shall be configurable from the server without reprogramming the device: the recognition decision threshold, the door unlock duration, the attendance cooldown period, the server request timeout, and whether offline card access is permitted.

**MR-2.** The recognition model on the server shall be replaceable without changing the device firmware.

**MR-3.** The system shall log sufficient diagnostic information for a maintainer to determine the cause of a failed identification without physical access to the device.

---

### 3.9 Usability Requirements

**UR-1.** An attendee shall be able to use the system correctly on their first attempt without instruction.

**UR-2.** The device shall indicate a result within the time limits of PR-1 and PR-2, or shall indicate that it is working, so that the user is never left uncertain whether the device has responded.

**UR-3.** An administrator shall be able to enroll a new user in under three minutes.

---

## 4. Verification

Each requirement shall be verifiable. The following table shows how each category will be confirmed.

| Requirement group | Verification method |
|---|---|
| Functional (FR) | Functional testing against each requirement, on assembled hardware |
| Performance (PR) | Timed measurement on the target hardware under representative load |
| Accuracy (AR) | Statistical testing with a test population under site lighting conditions |
| Security (SR) | Inspection, network capture analysis, and penetration testing of the administration interface |
| Reliability (RR) | Fault injection — network disconnection, power cycling, peripheral disconnection |
| Usability (UR) | Observation of untrained users completing the task |

Accuracy requirements in particular shall not be considered met on the basis of published model benchmarks. They shall be measured on this system, with this camera, at the installation site.

---

## 5. Open Issues

These must be resolved before the design is finalized. Each has been raised because it materially affects whether the requirements above can be met.

| ID | Issue | Why it matters |
|---|---|---|
| OI-1 | The camera specified in CON-3 has limited low-light performance. | May prevent AR-1 and AR-2 being met at an entrance in evening conditions. Requires early testing; may require supplementary illumination or a different camera. |
| OI-2 | Available bandwidth between the device and the network module has not been measured. | Directly determines whether PR-1 can be met when transmitting an image. |
| OI-3 | Available flash for offline buffering has not been quantified against CON-9. | Determines whether RR-5 is achievable, and whether face events can be buffered at all. |
| OI-4 | The retention period for attendance records (DR-7) has not been set. | Depends on site policy and applicable data protection law. |
| OI-5 | Whether the site requires protection against presentation attacks (SR-7). | If it does, the current scope is insufficient and liveness detection must be added to scope. |
