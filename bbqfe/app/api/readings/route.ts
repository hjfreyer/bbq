
import admin from 'firebase-admin';
import { FieldValue } from 'firebase-admin/firestore';


if (admin.apps.length === 0) {
  if (process.env.GCP_CREDS) {
    admin.initializeApp({
      credential: admin.credential.cert(JSON.parse(process.env.GCP_CREDS))
    });

  } else if (process.env.BBQ_FIREBASE_SERVICE_ACCOUNT_KEY) {
    admin.initializeApp({
      credential: admin.credential.cert(JSON.parse(process.env.BBQ_FIREBASE_SERVICE_ACCOUNT_KEY))
    });
  }
}


export const dynamic = 'force-dynamic'; // static by default, unless reading the request

type Readings = {
  session: number,
  food_temp_f: number,
  ambient_temp_f: number,
  duty_pct: number,
};

export async function POST(request: Request) {
  const readings = await request.json() as Readings;

  const db = admin.firestore();

  const session_ref = db.collection("sessions-2").doc("" + readings.session);
  const now = new Date();

  session_ref.set({ last_update: now });

  session_ref.collection("readings").doc('' + now.getTime()).set({
    time: now,
    duty_pct: readings.duty_pct,
    ambient_temp_f: readings.ambient_temp_f,
    food_temp_f: readings.food_temp_f,
  });

  return new Response(`ok\n`);
}
