
import { auth } from "../lib/auth";
import { db } from "./index";
import { user } from "./schema";
import { eq } from "drizzle-orm";

async function main() {
    // Generate a secure random password
    const password = "test@123";
    const email = "admin@example.com";
    const name = "Admin";

    console.log("🌱 Seeding database...");

    try {
        // Use Better Auth API to create the user
        let newUserId: string | undefined;
        try {
             const res = await auth.api.signUpEmail({
                body: {
                    email,
                    password,
                    name,
                }
            });
            if (res) {
                 newUserId = res.user.id;
                 console.log("✅ Admin user created via Auth API");
            }
        } catch (error: any) {
             if (String(error).includes("User already exists") || String(error).includes("UNIQUE constraint failed") || error?.body?.code === "USER_ALREADY_EXISTS") {
                 console.log("⚠️  Admin user already exists. Updating role...");
             } else {
                 throw error;
             }
        }

        // Update role to admin
        // Find user by email to be sure
        const existingUser = await db.select().from(user).where(eq(user.email, email)).get();
        
        if (existingUser) {
             await db.update(user).set({ role: 'admin' }).where(eq(user.email, email));
             console.log(`✅ Role updated to 'admin' for ${email}`);
        }

        if (newUserId) {
            console.log("\n----------------------------------------");
            console.log(`📧 Email:    ${email}`);
            console.log(`🔑 Password: ${password}`);
            console.log("----------------------------------------");
        }

    } catch (error) {
        console.error("\n❌ Error seeding admin user:", error);
    }
}

main();
