import altair as alt
import pandas as pd
import requests
import sqlite3
import streamlit as st
from streamlit_autorefresh import st_autorefresh

st.set_page_config(page_title="AI Fire Detection Dashboard", layout="wide")

API_BASE_URL = "http://127.0.0.1:8000"
LOCAL_DB_PATH = "macro_alerts.db"


def format_prediction_with_confidence(prediction, confidence):
    try:
        conf_value = float(confidence)
        if conf_value < 0:
            conf_value = 0.0
        if conf_value > 1:
            conf_value = 1.0
        return f"{prediction} ({conf_value * 100:.0f}%)"
    except (TypeError, ValueError):
        return prediction


def chart_theme(chart):
    return (
        chart.configure_view(fill="#0b2333", stroke="#3c6e8f")
        .configure_axis(
            labelColor="#e8f7ff",
            titleColor="#8be9ff",
            gridColor="#315067",
            tickColor="#9bd1ec",
            domainColor="#6ba4c4",
            labelFontSize=12,
            titleFontSize=13,
        )
        .configure_legend(
            labelColor="#e8f7ff",
            titleColor="#8be9ff",
            labelFontSize=12,
            titleFontSize=13,
        )
        .configure_title(color="#8be9ff", fontSize=14)
    )


def render_sensor_chart(trend_data, y_col, title, color, mark_type="line"):
    chart_data = trend_data.reset_index()[["created_at", y_col]].rename(columns={y_col: "value"})

    base = alt.Chart(chart_data).encode(
        x=alt.X("created_at:T", title="Time"),
        y=alt.Y("value:Q", title=title),
        tooltip=[
            alt.Tooltip("created_at:T", title="Time"),
            alt.Tooltip("value:Q", title=title, format=".2f"),
        ],
    )

    if mark_type == "area":
        chart = base.mark_area(color=color, opacity=0.45)
    elif mark_type == "bar":
        chart = base.mark_bar(color=color, opacity=0.9)
    else:
        chart = base.mark_line(color=color, strokeWidth=3, point=True)

    st.altair_chart(chart_theme(chart).properties(height=220), use_container_width=True)


def style_severity_score(value):
    try:
        score = float(value)
    except (TypeError, ValueError):
        return ""

    if score <= 20:
        return "background-color: #4cd37b; color: #06230f; font-weight: 700;"
    if score <= 50:
        return "background-color: #ffd166; color: #2a1a00; font-weight: 700;"
    if score <= 80:
        return "background-color: #ff9f43; color: #2a1100; font-weight: 700;"
    return "background-color: #ff5d73; color: #33030c; font-weight: 700;"


def style_emergency_level(value):
    palette = {
        "Low": "background-color: #6bd99e; color: #052016; font-weight: 700;",
        "Medium": "background-color: #ffd166; color: #2a1a00; font-weight: 700;",
        "High": "background-color: #ff9f43; color: #2a1100; font-weight: 700;",
        "Critical": "background-color: #ff5d73; color: #33030c; font-weight: 700;",
    }
    return palette.get(str(value), "")


def style_severity_band(value):
    palette = {
        "Safe": "background-color: #4cd37b; color: #06230f; font-weight: 700;",
        "Warning": "background-color: #ffd166; color: #2a1a00; font-weight: 700;",
        "High Risk": "background-color: #ff9f43; color: #2a1100; font-weight: 700;",
        "Critical": "background-color: #ff5d73; color: #33030c; font-weight: 700;",
    }
    return palette.get(str(value), "")


def style_prediction_cell(value):
    text = str(value)
    if "Fire Emergency" in text:
        return "background-color: #ff5d73; color: #33030c; font-weight: 700;"
    if "Gas Leakage" in text:
        return "background-color: #ff9f43; color: #2a1100; font-weight: 700;"
    if "Smoke Warning" in text:
        return "background-color: #ffd166; color: #2a1a00; font-weight: 700;"
    if "False Alarm" in text or "No Fire" in text:
        return "background-color: #6bd99e; color: #052016; font-weight: 700;"
    return ""


def style_events_table(df):
    styler = (
        df.style.format(
            {
                "Temperature": "{:.1f}",
                "Humidity": "{:.1f}",
                "Smoke/Gas": "{:.0f}",
                "Flame": "{:.0f}",
                "Severity Score": "{:.0f}",
            },
            na_rep="-",
        )
        .set_properties(**{"background-color": "#0f2437", "color": "#eaf8ff"})
    )

    if "Prediction" in df.columns:
        styler = styler.map(style_prediction_cell, subset=["Prediction"])

    if "Severity Score" in df.columns:
        styler = styler.map(style_severity_score, subset=["Severity Score"])
    if "Severity Band" in df.columns:
        styler = styler.map(style_severity_band, subset=["Severity Band"])
    if "Emergency Level" in df.columns:
        styler = styler.map(style_emergency_level, subset=["Emergency Level"])

    return styler.hide(axis="index")


def inject_styles():
    st.markdown(
        """
        <style>
            @import url('https://fonts.googleapis.com/css2?family=Sora:wght@400;600;700&family=JetBrains+Mono:wght@500&display=swap');

            :root {
                --bg-1: #081a24;
                --bg-2: #102f3e;
                --panel: rgba(255, 255, 255, 0.09);
                --line: rgba(255, 255, 255, 0.20);
                --text: #f0f7fb;
                --muted: #b8d4e2;
            }

            .stApp {
                background:
                    radial-gradient(circle at 12% 8%, rgba(255, 99, 99, 0.16), transparent 30%),
                    radial-gradient(circle at 90% 0%, rgba(255, 190, 70, 0.14), transparent 25%),
                    linear-gradient(140deg, var(--bg-1), var(--bg-2));
                color: var(--text);
                font-family: 'Sora', sans-serif;
            }

            .topbar {
                border: 1px solid var(--line);
                border-radius: 16px;
                padding: 0.9rem 1rem;
                background: linear-gradient(110deg, rgba(255, 99, 99, 0.15), rgba(10, 30, 44, 0.5));
                margin-bottom: 0.9rem;
            }

            .kpi {
                border: 1px solid var(--line);
                border-radius: 14px;
                background: var(--panel);
                padding: 0.8rem 1rem;
                min-height: 95px;
            }

            .kpi-title {
                color: var(--muted);
                font-size: 0.82rem;
            }

            .kpi-value {
                font-size: 1.66rem;
                font-weight: 700;
            }

            .kpi-foot {
                color: var(--muted);
                font-family: 'JetBrains Mono', monospace;
                font-size: 0.72rem;
            }

            .alert-card {
                border: 1px solid var(--line);
                border-left: 7px solid #ffbf3c;
                border-radius: 12px;
                padding: 0.8rem 0.9rem;
                background: rgba(255, 255, 255, 0.08);
            }

            .pill {
                display: inline-block;
                padding: 0.26rem 0.58rem;
                border-radius: 999px;
                border: 1px solid var(--line);
                margin-right: 0.4rem;
                font-size: 0.78rem;
            }

            .fancy-table-wrap {
                border: 1px solid rgba(255, 255, 255, 0.2);
                border-radius: 14px;
                overflow-x: auto;
                background: linear-gradient(145deg, rgba(8, 26, 36, 0.95), rgba(16, 47, 62, 0.92));
                padding: 0.35rem;
            }

            .fancy-table-wrap table {
                border-collapse: collapse;
                width: 100%;
                min-width: 980px;
                font-size: 0.94rem;
            }

            .fancy-table-wrap thead th {
                background: linear-gradient(120deg, #15384e, #1a4964);
                color: #eef9ff;
                border: 1px solid rgba(255, 255, 255, 0.16);
                padding: 0.55rem 0.5rem;
                text-align: left;
                position: sticky;
                top: 0;
                z-index: 2;
            }

            .fancy-table-wrap tbody td {
                border: 1px solid rgba(255, 255, 255, 0.11);
                padding: 0.48rem 0.5rem;
                color: #eaf8ff;
            }

            .fancy-table-wrap tbody tr:nth-child(odd) td {
                background-color: rgba(16, 38, 58, 0.96);
            }

            .fancy-table-wrap tbody tr:nth-child(even) td {
                background-color: rgba(11, 30, 47, 0.96);
            }

            .fancy-table-wrap tbody tr:hover td {
                background-color: rgba(34, 74, 102, 0.95) !important;
            }
        </style>
        """,
        unsafe_allow_html=True,
    )


def api_get(path):
    try:
        response = requests.get(f"{API_BASE_URL}{path}", timeout=4)
        response.raise_for_status()
        return response.json(), None
    except requests.RequestException as exc:
        return None, str(exc)


def load_local_events(limit):
    try:
        with sqlite3.connect(LOCAL_DB_PATH) as conn:
            conn.row_factory = sqlite3.Row
            rows = conn.execute(
                """
                SELECT id, created_at, device_id, temperature, humidity, smoke_gas_level,
                       flame_detected, initial_warning, prediction, prediction_confidence,
                       severity_score, severity_band, verification, emergency_level,
                       action, authority, notify_required
                FROM events
                ORDER BY id DESC
                LIMIT ?
                """,
                (limit,),
            ).fetchall()
        return [dict(row) for row in rows], None
    except sqlite3.Error as exc:
        return [], str(exc)


def calculate_local_stats(events):
    total_events = len(events)
    false_alarms = sum(1 for event in events if event.get("prediction") == "False Alarm")
    critical_events = sum(1 for event in events if event.get("prediction") == "Fire Emergency")
    return {
        "total_events": total_events,
        "false_alarms": false_alarms,
        "real_alerts": total_events - false_alarms,
        "critical_events": critical_events,
    }


def render_kpi(title, value, footnote):
    st.markdown(
        f"""
        <div class="kpi">
            <div class="kpi-title">{title}</div>
            <div class="kpi-value">{value}</div>
            <div class="kpi-foot">{footnote}</div>
        </div>
        """,
        unsafe_allow_html=True,
    )


inject_styles()

st.markdown(
    """
    <div class="topbar">
        <h2 style="margin:0;">AI-Enabled IoT-Based Intelligent Fire Detection and Early Warning System</h2>
    </div>
    """,
    unsafe_allow_html=True,
)

st.sidebar.header("Controls")
refresh_clicked = st.sidebar.button("Refresh Now", use_container_width=True)
limit = st.sidebar.slider("History Size", min_value=50, max_value=500, value=200, step=50)
auto_refresh_enabled = st.sidebar.toggle("Auto Refresh", value=True)
auto_refresh_seconds = st.sidebar.slider(
    "Refresh Interval (seconds)",
    min_value=1,
    max_value=30,
    value=5,
    step=1,
)

if auto_refresh_enabled:
    st_autorefresh(interval=auto_refresh_seconds * 1000, key="dashboard_auto_refresh")
    st.sidebar.caption(f"Live updates every {auto_refresh_seconds} seconds")
else:
    st.sidebar.caption("Auto refresh paused")

health, health_error = api_get("/health")
backend_online = health_error is None

if refresh_clicked:
    st.rerun()

if backend_online:
    st.markdown(
        "<span class='pill'>Backend Online</span>"
        f"<span class='pill'>{health.get('service', 'macro-project-backend')}</span>",
        unsafe_allow_html=True,
    )

    stats, stats_error = api_get("/api/v1/stats")
    if stats_error:
        stats = {"total_events": 0, "false_alarms": 0, "real_alerts": 0, "critical_events": 0}

    events_data, events_error = api_get(f"/api/v1/events?limit={limit}")
    if events_error:
        st.warning("Live backend fetch failed. Falling back to local database cache.")
        events, local_error = load_local_events(limit)
        if local_error:
            st.error(f"Backend unavailable and local database read failed: {local_error}")
            st.stop()
        stats = calculate_local_stats(events)
    else:
        events = events_data.get("events", [])
        if stats_error:
            stats = calculate_local_stats(events)
else:
    st.markdown(
        "<span class='pill'>Backend Offline</span><span class='pill'>Local Logic Mode</span>",
        unsafe_allow_html=True,
    )
    st.warning(
        "Backend unavailable. Showing locally stored events while ESP32 continues hardware alarms using local logic."
    )
    events, local_error = load_local_events(limit)
    if local_error:
        st.error(f"Could not read local database: {local_error}")
        st.info("Start backend once to initialize database, then send telemetry from ESP32.")
        st.stop()
    stats = calculate_local_stats(events)

df_events = pd.DataFrame(events)

k1, k2, k3, k4 = st.columns(4)
with k1:
    render_kpi("Total Events", stats["total_events"], "all logged readings")
with k2:
    render_kpi("False Alarms", stats["false_alarms"], "not escalated")
with k3:
    render_kpi("Real Alerts", stats["real_alerts"], "validated incidents")
with k4:
    render_kpi("Critical", stats["critical_events"], "fire emergencies")

if df_events.empty:
    st.info("No sensor readings received yet. Start ESP32 and send telemetry.")
    st.stop()

df_events["created_at"] = pd.to_datetime(df_events["created_at"], errors="coerce")
df_events = df_events.sort_values("created_at")

latest = df_events.iloc[-1]
st.caption(
    f"Live view refreshed at {pd.Timestamp.now().strftime('%Y-%m-%d %H:%M:%S')} | "
    f"Latest event time: {latest.get('created_at')}"
)

latest_prediction_display = format_prediction_with_confidence(
    latest.get("prediction", "Unknown"), latest.get("prediction_confidence")
)
latest_severity_score = latest.get("severity_score", 0)
latest_severity_band = latest.get("severity_band", "Safe")
alert_color = "#ffbf3c"
if latest["emergency_level"] == "Critical":
    alert_color = "#ff5f5f"
elif latest["emergency_level"] == "Low":
    alert_color = "#35d38a"

left, right = st.columns([1.15, 1])

with left:
    st.subheader("Latest Sensor Alert")
    st.markdown(
        f"""
        <div class="alert-card" style="border-left-color:{alert_color};">
            <strong>{latest_prediction_display}</strong><br>
            Timestamp: {latest['created_at']}<br>
            Device: {latest.get('device_id', 'N/A')}<br>
            Emergency Level: {latest['emergency_level']}<br>
            Severity Score: {latest_severity_score} ({latest_severity_band})<br>
            Initial Warning: {latest['initial_warning']}<br>
            Action: {latest['action']}<br>
            Authority: {latest['authority']}
        </div>
        """,
        unsafe_allow_html=True,
    )

    st.subheader("Sensor Trend Charts")
    trend_df = df_events[
        ["created_at", "temperature", "humidity", "smoke_gas_level", "flame_detected"]
    ].dropna()
    if trend_df.empty:
        st.info("No trend data available yet.")
    else:
        trend_df = trend_df.sort_values("created_at").set_index("created_at")

        t_col, h_col = st.columns(2)
        with t_col:
            st.caption("Temperature")
            render_sensor_chart(trend_df, "temperature", "Temperature", "#ff7a59", "line")

        with h_col:
            st.caption("Humidity")
            render_sensor_chart(trend_df, "humidity", "Humidity", "#4ecdc4", "area")

        s_col, f_col = st.columns(2)
        with s_col:
            st.caption("Smoke/Gas Level")
            render_sensor_chart(trend_df, "smoke_gas_level", "Smoke/Gas", "#ff4d6d", "bar")

        with f_col:
            st.caption("Flame Detection (0/1)")
            render_sensor_chart(trend_df, "flame_detected", "Flame", "#9ef01a", "line")

with right:
    st.subheader("Alert Class Distribution")
    class_counts = (
        df_events["prediction"].value_counts().rename_axis("Alert").reset_index(name="Count")
    )
    class_chart = alt.Chart(class_counts).mark_bar(cornerRadiusTopLeft=6, cornerRadiusTopRight=6).encode(
        x=alt.X("Alert:N", sort="-y", title="Alert Class"),
        y=alt.Y("Count:Q", title="Count"),
        color=alt.Color("Alert:N", scale=alt.Scale(scheme="tableau20"), legend=None),
        tooltip=["Alert:N", "Count:Q"],
    )
    st.altair_chart(chart_theme(class_chart).properties(height=280), use_container_width=True)

    st.subheader("Emergency Level Distribution")
    level_counts = (
        df_events["emergency_level"].value_counts().rename_axis("Level").reset_index(name="Count")
    )
    level_chart = alt.Chart(level_counts).mark_arc(innerRadius=55).encode(
        theta=alt.Theta("Count:Q"),
        color=alt.Color(
            "Level:N",
            scale=alt.Scale(
                domain=["Low", "Medium", "High", "Critical"],
                range=["#6bd99e", "#ffd166", "#ff9f43", "#ff5d73"],
            ),
            legend=alt.Legend(title="Emergency Level"),
        ),
        tooltip=["Level:N", "Count:Q"],
    )
    st.altair_chart(chart_theme(level_chart).properties(height=280), use_container_width=True)

    st.subheader("High Priority Feed")
    high_df = df_events[df_events["emergency_level"].isin(["High", "Critical"])].tail(8)
    if high_df.empty:
        st.success("No high-priority alerts in recent readings.")
    else:
        for _, row in high_df.iterrows():
            prediction_text = format_prediction_with_confidence(
                row.get("prediction", "Unknown"), row.get("prediction_confidence")
            )
            severity_text = f"Severity {row.get('severity_score', 0)} ({row.get('severity_band', 'Safe')})"
            st.warning(
                f"{row['created_at']} | {prediction_text} | {severity_text} | Device: {row.get('device_id', 'N/A')}"
            )

st.subheader("Sensor Event Table")
table_df = df_events.rename(
    columns={
        "created_at": "Timestamp",
        "device_id": "Device",
        "temperature": "Temperature",
        "humidity": "Humidity",
        "smoke_gas_level": "Smoke/Gas",
        "flame_detected": "Flame",
        "initial_warning": "Initial Warning",
        "prediction": "Prediction",
        "prediction_confidence": "Confidence",
        "severity_score": "Severity Score",
        "severity_band": "Severity Band",
        "verification": "Verification",
        "emergency_level": "Emergency Level",
        "action": "Action",
        "authority": "Authority",
        "notify_required": "Notify",
    }
)

if "Prediction" in table_df.columns and "Confidence" in table_df.columns:
    table_df["Prediction"] = table_df.apply(
        lambda row: format_prediction_with_confidence(row["Prediction"], row["Confidence"]),
        axis=1,
    )
    table_df = table_df.drop(columns=["Confidence"])

styled_table_html = style_events_table(table_df).to_html()
st.markdown(f"<div class='fancy-table-wrap'>{styled_table_html}</div>", unsafe_allow_html=True)