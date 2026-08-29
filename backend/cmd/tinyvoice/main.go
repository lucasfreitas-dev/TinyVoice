package main

import (
	"context"
	"fmt"
	"os"
	"text/tabwriter"

	"github.com/spf13/cobra"

	"tinyvoice/backend/internal/config"
	"tinyvoice/backend/internal/conversation"
	"tinyvoice/backend/internal/database"
	"tinyvoice/backend/internal/device"
)

func main() {
	if err := rootCmd().Execute(); err != nil {
		os.Exit(1)
	}
}

func rootCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "tinyvoice",
		Short: "TinyVoice administration CLI",
	}
	cmd.AddCommand(deviceCmd(), conversationCmd())
	return cmd
}

func deviceCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "device", Short: "Manage devices"}
	cmd.AddCommand(deviceCreateCmd(), deviceListCmd(), deviceBindCmd())
	return cmd
}

func deviceCreateCmd() *cobra.Command {
	var name string
	cmd := &cobra.Command{
		Use:   "create",
		Short: "Create a new device",
		RunE: func(cmd *cobra.Command, args []string) error {
			if name == "" {
				return fmt.Errorf("--name is required")
			}
			if err := requireAdmin(); err != nil {
				return err
			}
			ctx := context.Background()
			svc, pool, err := deviceService(ctx)
			if err != nil {
				return err
			}
			defer pool.Close()

			result, err := svc.Create(ctx, name)
			if err != nil {
				return err
			}

			fmt.Printf("Device created\n")
			fmt.Printf("  ID:    %s\n", result.Device.ID)
			fmt.Printf("  Name:  %s\n", result.Device.Name)
			fmt.Printf("  Token: %s\n", result.Token)
			fmt.Println("\nSave the token now — it will not be shown again.")
			return nil
		},
	}
	cmd.Flags().StringVar(&name, "name", "", "Device name")
	return cmd
}

func deviceListCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "list",
		Short: "List devices",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := requireAdmin(); err != nil {
				return err
			}
			ctx := context.Background()
			svc, pool, err := deviceService(ctx)
			if err != nil {
				return err
			}
			defer pool.Close()

			devices, err := svc.List(ctx)
			if err != nil {
				return err
			}

			w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
			fmt.Fprintln(w, "ID\tNAME\tENABLED\tLAST SEEN")
			for _, d := range devices {
				lastSeen := "-"
				if d.LastSeenAt != nil {
					lastSeen = d.LastSeenAt.Format("2006-01-02 15:04:05")
				}
				fmt.Fprintf(w, "%s\t%s\t%v\t%s\n", d.ID, d.Name, d.Enabled, lastSeen)
			}
			return w.Flush()
		},
	}
}

func deviceBindCmd() *cobra.Command {
	var deviceID, conversationID string
	cmd := &cobra.Command{
		Use:   "bind",
		Short: "Bind a device to a conversation",
		RunE: func(cmd *cobra.Command, args []string) error {
			if deviceID == "" || conversationID == "" {
				return fmt.Errorf("--device and --conversation are required")
			}
			if err := requireAdmin(); err != nil {
				return err
			}
			ctx := context.Background()
			svc, pool, err := deviceService(ctx)
			if err != nil {
				return err
			}
			defer pool.Close()

			if err := svc.BindConversation(ctx, deviceID, conversationID); err != nil {
				return err
			}
			fmt.Println("Device bound to conversation.")
			return nil
		},
	}
	cmd.Flags().StringVar(&deviceID, "device", "", "Device UUID")
	cmd.Flags().StringVar(&conversationID, "conversation", "", "Conversation UUID")
	return cmd
}

func conversationCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "conversation", Short: "Manage conversations"}
	cmd.AddCommand(conversationCreateCmd(), conversationListCmd())
	return cmd
}

func conversationCreateCmd() *cobra.Command {
	var name, recipient string
	cmd := &cobra.Command{
		Use:   "create",
		Short: "Create a conversation",
		RunE: func(cmd *cobra.Command, args []string) error {
			if name == "" || recipient == "" {
				return fmt.Errorf("--name and --recipient are required")
			}
			if err := requireAdmin(); err != nil {
				return err
			}
			ctx := context.Background()
			cfg, err := config.Load()
			if err != nil {
				return err
			}
			pool, err := database.NewPool(ctx, cfg.DatabaseURL)
			if err != nil {
				return err
			}
			defer pool.Close()

			svc := conversation.NewService(conversation.NewRepository(pool))
			c, err := svc.Create(ctx, name, recipient)
			if err != nil {
				return err
			}
			fmt.Printf("Conversation created\n  ID:        %s\n  Name:      %s\n  Recipient: %s\n", c.ID, c.Name, c.WhatsAppRecipient)
			return nil
		},
	}
	cmd.Flags().StringVar(&name, "name", "", "Conversation name")
	cmd.Flags().StringVar(&recipient, "recipient", "", "WhatsApp recipient number e.g. 5511999999999")
	return cmd
}

func conversationListCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "list",
		Short: "List conversations",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := requireAdmin(); err != nil {
				return err
			}
			ctx := context.Background()
			cfg, err := config.Load()
			if err != nil {
				return err
			}
			pool, err := database.NewPool(ctx, cfg.DatabaseURL)
			if err != nil {
				return err
			}
			defer pool.Close()

			items, err := conversation.NewService(conversation.NewRepository(pool)).List(ctx)
			if err != nil {
				return err
			}
			w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
			fmt.Fprintln(w, "ID\tNAME\tRECIPIENT")
			for _, c := range items {
				fmt.Fprintf(w, "%s\t%s\t%s\n", c.ID, c.Name, c.WhatsAppRecipient)
			}
			return w.Flush()
		},
	}
}

func requireAdmin() error {
	if os.Getenv("ADMIN_TOKEN") == "" {
		return fmt.Errorf("ADMIN_TOKEN environment variable is required")
	}
	return nil
}

func deviceService(ctx context.Context) (*device.Service, interface{ Close() }, error) {
	cfg, err := config.Load()
	if err != nil {
		return nil, nil, err
	}
	pool, err := database.NewPool(ctx, cfg.DatabaseURL)
	if err != nil {
		return nil, nil, err
	}
	return device.NewService(device.NewRepository(pool)), pool, nil
}
