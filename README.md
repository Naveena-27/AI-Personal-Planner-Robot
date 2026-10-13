\# AI-Powered Personal Planner Robot



An ESP32-based AI personal planner robot designed to manage daily schedules, alarms, and reminders through natural voice interaction.



The system combines embedded hardware, IoT communication, speech recognition, AI-based natural language processing, and a FastAPI backend to create an interactive personal scheduling assistant.



\## Features



\- Voice-based interaction

\- AI-powered natural language command processing

\- Create, update, and cancel scheduled events

\- Alarm scheduling and physical alarm control

\- Daily plan summaries

\- Speech-to-Text (STT)

\- Text-to-Speech (TTS) responses

\- OLED display for robot expressions and status

\- LDR-based automatic sleep/wake behaviour

\- Physical push button for alarm and interaction control

\- Wi-Fi communication between ESP32 and backend

\- Web interface for viewing and managing schedules

\- SQLite database for storing events and alarms



\## System Architecture



The project consists of three main components:



\### ESP32 Robot

Handles the physical interaction with the user.



Hardware includes:



\- ESP32

\- INMP441 I2S Microphone

\- MAX98357A I2S Audio Amplifier

\- 3W Speaker

\- 0.96" OLED Display

\- LDR Light Sensor

\- Push Button



\### Backend



The backend is developed using Python and FastAPI.



It handles:



\- Schedule management

\- Alarm management

\- Speech processing

\- AI command interpretation

\- Database operations

\- Communication with the ESP32



\### Web Interface



A web-based interface allows the user to:



\- View scheduled events

\- Create new plans

\- Update existing plans

\- Delete plans

\- Manage alarms



\## Technologies Used



\### Hardware

\- ESP32

\- INMP441 Microphone

\- MAX98357A Audio Amplifier

\- OLED Display

\- LDR

\- Push Button



\### Software

\- Arduino / C++

\- Python

\- FastAPI

\- SQLite

\- HTML

\- CSS

\- JavaScript

\- REST API

\- Wi-Fi / HTTP

\- Speech-to-Text

\- Text-to-Speech

\- Large Language Model (LLM)



\## Example Voice Commands



\- "What are my plans for today?"

\- "Schedule a team meeting today at 1 PM."

\- "Move my team meeting from 1 PM to 3 PM."

\- "Cancel my dental appointment."

\- "What time is my family outing?"



The AI interprets the user's request and performs the corresponding scheduling operation through the backend.



\## Project Structure



&#x20;   AI-Personal-Planner-Robot/

&#x20;   |

&#x20;   |-- robot\_final/

&#x20;   |   `-- robot\_final.ino

&#x20;   |

&#x20;   |-- assistant-backend/

&#x20;   |   |-- main.py

&#x20;   |   |-- assistant.py

&#x20;   |   |-- voice.py

&#x20;   |   |-- db.py

&#x20;   |   |-- requirements.txt

&#x20;   |   `-- static/

&#x20;   |       `-- index.html

&#x20;   |

&#x20;   |-- .gitignore

&#x20;   `-- README.md



\## Security



Sensitive configuration files such as `.env`, API keys, the local database, and Python virtual environment are excluded from the repository using `.gitignore`.



\## Future Improvements



\- Custom enclosure for the robot

\- Improved voice-response latency

\- Cloud deployment of the backend

\- Remote access outside the local network

\- Additional smart assistant capabilities



\## Author



\*\*Naveena Rajendran\*\*



Software Engineering Undergraduate  

University of Sri Jayewardenepura

