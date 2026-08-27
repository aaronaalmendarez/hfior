# Terminology

| Term | Meaning |
| --- | --- |
| Observation | One movement update from the input source. |
| Record | One saved observation with a number and time. Records cannot change after they are published. |
| History | The ordered records that have not yet been read by an application. |
| Producer | The part that writes records. This may be a compositor, input library, or bridge. |
| Consumer | The approved application or system part that reads the records. |
| Expensive action | Heavier work such as waking a thread, calling game logic, or updating the camera. |
| Useful-sample age | How old the newest input is when the game actually uses it. Lower is fresher. |
| Late latch | One final quick read for movement that arrived just before the game uses input. |
| Urgent transition | A change that should arrive immediately, such as a button press or release. |
| Requested rate | The polling rate selected for the device. It may not be the rate the system actually receives. |
| Observed rate | The report rate the test actually measured. |

“Event,” “packet,” and “report” keep their normal hardware or operating-system
meanings. An HFIOR record is the saved form used inside HFIOR.
